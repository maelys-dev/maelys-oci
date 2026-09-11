#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""OCI conformance and fail-closed store regressions, using private fixtures."""
import copy
import gzip
import json
import pathlib
import shutil
import subprocess
import tarfile
import tempfile

from test_oci_materializer_adversarial import (
    descriptor, digest, imported_artifact, layout, run_helper, run_import, tar_bytes,
)

LAYER = "application/vnd.oci.image.layer.v1.tar"
CONFIG = "application/vnd.oci.image.config.v1+json"
MANIFEST = "application/vnd.oci.image.manifest.v1+json"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def rewrite_config(source, change, entry_index=0):
    blobs = source / "blobs" / "sha256"
    index = json.loads((source / "index.json").read_bytes())
    entry = index["manifests"][entry_index]
    manifest = json.loads((blobs / entry["digest"][7:]).read_bytes())
    config = json.loads((blobs / manifest["config"]["digest"][7:]).read_bytes())
    change(config)
    if "platform" in entry:
        entry["platform"].update(os=config["os"], architecture=config["architecture"])
    encoded = canonical(config)
    (blobs / digest(encoded)[7:]).write_bytes(encoded)
    manifest["config"] = descriptor(encoded, CONFIG)
    encoded = canonical(manifest)
    (blobs / digest(encoded)[7:]).write_bytes(encoded)
    index["manifests"][entry_index] = descriptor(
        encoded, MANIFEST, entry.get("platform"))
    (source / "index.json").write_bytes(canonical(index))


def layer_blob(source, index=0, entry_index=0):
    """The blob of one declared layer, never whichever blob iterdir yields."""
    blobs = source / "blobs" / "sha256"
    manifests = json.loads((source / "index.json").read_bytes())["manifests"]
    manifest = json.loads((blobs / manifests[entry_index]["digest"][7:]).read_bytes())
    return blobs / manifest["layers"][index]["digest"][7:]


def assert_refused_content(process):
    """Hostile or invalid content is a protocol failure, never a retry hint."""
    assert process.returncode == 1, process.stderr
    error = json.loads(process.stderr)["error"]
    assert error["code"] == "PROTOCOL_FAILED", error
    assert "Retry" not in error["hint"], error


def assert_unpublished(store):
    assert not list(store.glob("objects/*/*/artifact.seal"))
    assert not list(store.glob("sources/*/*/closure.json"))
    assert not list(store.glob("tmp/import/*"))


def assert_roots(artifact, expected, absent):
    with tarfile.open(artifact / "rootfs.tar") as archive:
        actual = {member.name: archive.extractfile(member).read()
                  for member in archive if member.isfile() or member.islnk()}
        assert actual == expected, actual
        assert all(not pathlib.PurePosixPath(m.name).name.startswith(".wh.")
                   for m in archive.getmembers())
    debugfs = shutil.which("debugfs") or "/opt/homebrew/opt/e2fsprogs/sbin/debugfs"
    for name, content in expected.items():
        result = subprocess.run([debugfs, "-R", f"cat /{name}",
                                 str(artifact / "root.ext4")], capture_output=True)
        assert result.returncode == 0 and result.stdout == content, result
    for name in absent:
        result = subprocess.run([debugfs, "-R", f"stat /{name}",
                                 str(artifact / "root.ext4")], capture_output=True)
        assert b"File not found" in result.stderr, result


def whiteouts(base):
    cases = [
        ([("same", "file", b"old"), ("kept", "file", b"kept")],
         [("same", "file", b"new"), ("alias", "hardlink", "same")],
         ".wh.same", {"same": b"new", "alias": b"new", "kept": b"kept"}, []),
        ([("dir/old", "file", b"old")], [("dir/new", "file", b"new")],
         "dir/.wh..wh..opq", {"dir/new": b"new"}, ["dir/old"]),
        ([("dir/old", "file", b"old")], [("dir/new", "file", b"new")],
         ".wh.dir", {"dir/new": b"new"}, ["dir/old"]),
        ([("old", "file", b"old")], [("new", "file", b"new")],
         ".wh..wh..opq", {"new": b"new"}, ["old"]),
    ]
    for i, (lower, additions, marker, expected, absent) in enumerate(cases):
        roots = []
        for first in (True, False):
            name = f"whiteout-{i}-{first}"
            source, store = base / name, base / (name + "-store")
            # PAX path overrides and ordinary headers must have identical semantics.
            whiteout = [("marker", "file", b"", 0o644, {"path": marker})]
            upper = whiteout + additions if first else additions + whiteout
            layout(source, [tar_bytes(lower), tar_bytes(upper, pax=True)])
            artifact = imported_artifact(run_import(source, store))
            assert_roots(artifact, expected, absent)
            roots.append(digest((artifact / "rootfs.tar").read_bytes()))
        assert roots[0] == roots[1]
    for i, marker in enumerate([
        (".wh.file", "dir", b""), (".wh.file", "file", b"data"),
        (".wh.file", "symlink", "file"), (".wh.file", "hardlink", "file"),
        (".wh..wh..opq", "dir", b""), (".wh..", "file", b""),
        (".wh...", "file", b""),
    ]):
        source, store = base / f"marker-{i}", base / f"marker-store-{i}"
        layout(source, [tar_bytes([("file", "file", b"data"), marker])])
        assert_refused_content(run_import(source, store, ok=False))
        assert_unpublished(store)
    # An interior marker segment is never consumed as a whiteout: materialized,
    # its name would be read back as a deletion by a consumer of rootfs.tar.
    for i, entries in enumerate([
        [("a", "dir", b""), ("a/.wh.b/c", "file", b"x")],
        [("a", "dir", b""), ("a/.wh..wh..opq/c", "file", b"x")],
    ]):
        source, store = base / f"interior-{i}", base / f"interior-store-{i}"
        layout(source, [tar_bytes(entries)])
        assert_refused_content(run_import(source, store, ok=False))
        assert_unpublished(store)


def configs_and_encodings(base):
    raw = tar_bytes([("file", "file", b"data")])
    changes = [
        lambda c: c.pop("rootfs"),
        lambda c: c.update(rootfs=None),
        lambda c: c["rootfs"].update(type="foreign"),
        lambda c: c["rootfs"].pop("diff_ids"),
        lambda c: c["rootfs"].update(diff_ids=[]),
        lambda c: c["rootfs"].update(diff_ids=[digest(raw)] * 2),
        lambda c: c["rootfs"].update(diff_ids=digest(raw)),
        lambda c: c["rootfs"].update(diff_ids=[7]),
        lambda c: c["rootfs"].update(diff_ids=["sha256:short"]),
        lambda c: c["rootfs"].update(diff_ids=[digest(raw).upper()]),
        lambda c: c["rootfs"].update(diff_ids=[digest(raw) + "\0"]),
        lambda c: c.update(os="windows"),
        lambda c: c.update(architecture="riscv64"),
        lambda c: c.update(variant="v9"),
    ]
    for i, change in enumerate(changes):
        source, store = base / f"config-{i}", base / f"config-store-{i}"
        layout(source, [raw])
        rewrite_config(source, change)
        plan = run_helper("import", str(source), "--store", str(store))
        assert plan.returncode == 1 and not store.exists(), plan.stderr
        assert json.loads(plan.stderr)["error"]["code"] == "PROTOCOL_FAILED"
        run_import(source, store, ok=False)
        assert_unpublished(store)
    source, store = base / "index-variant", base / "index-variant-store"
    layout(source, [raw])
    index = json.loads((source / "index.json").read_bytes())
    index["manifests"][0]["platform"]["variant"] = "v9"
    (source / "index.json").write_bytes(canonical(index))
    run_import(source, store, ok=False)
    assert_unpublished(store)
    for suffix, omit_platform in (("declared", False), ("implicit", True)):
        source = base / f"mixed-platform-{suffix}"
        store = base / f"mixed-platform-store-{suffix}"
        layout(source, [raw], architectures=("arm64", "amd64"))
        rewrite_config(source, lambda c: c.update(os="windows"), entry_index=1)
        if omit_platform:
            index = json.loads((source / "index.json").read_bytes())
            index["manifests"][1].pop("platform")
            (source / "index.json").write_bytes(canonical(index))
        inspected = run_helper("inspect", str(source))
        assert inspected.returncode == 0, inspected.stderr
        manifests = json.loads(inspected.stdout)["data"]["manifests"]
        assert [item["platform"] for item in manifests] == ["linux/arm64"]
        selected = run_import(source, store, "--platform", "linux/arm64")
        assert json.loads(selected.stdout)["data"]["platform"] == "linux/arm64"
    for i, ids in enumerate([[digest(b"different")], [digest(raw)] * 2]):
        source, store = base / f"diffid-{i}", base / f"diffid-store-{i}"
        layers = [raw] if i == 0 else [raw, tar_bytes([("second", "file", b"2")])]
        layout(source, layers)
        rewrite_config(source, lambda c: c["rootfs"].update(diff_ids=ids))
        assert_refused_content(run_import(source, store, ok=False))
        assert_unpublished(store)
    source, store = base / "diffid-order", base / "diffid-order-store"
    layout(source, [raw, tar_bytes([("second", "file", b"2")])])
    rewrite_config(source, lambda c: c["rootfs"]["diff_ids"].reverse())
    assert_refused_content(run_import(source, store, ok=False))
    assert_unpublished(store)
    for i, (body, media, valid) in enumerate([
        (raw, LAYER, True), (gzip.compress(raw), LAYER + "+gzip", True),
        (gzip.compress(raw), "application/vnd.docker.image.rootfs.diff.tar.gzip", True),
        (gzip.compress(raw), LAYER, False), (raw, LAYER + "+gzip", False),
        (gzip.compress(raw), LAYER + "+zstd", False),
        (gzip.compress(gzip.compress(raw)), LAYER + "+gzip", False),
    ]):
        source, store = base / f"codec-{i}", base / f"codec-store-{i}"
        layout(source, [body], layer_media_type=media)
        process = run_import(source, store, ok=valid)
        if valid:
            assert_roots(imported_artifact(process), {"file": b"data"}, [])
        else:
            assert_refused_content(process)
            assert_unpublished(store)
    # A layer blob larger than its descriptor declares is the source's fault:
    # the manifest and the config stay intact, so only the bounded copy of the
    # layer can refuse it.
    for i, layers in enumerate(([raw], [raw, tar_bytes([("second", "file", b"2")])])):
        source, store = base / f"oversize-{i}", base / f"oversize-store-{i}"
        layout(source, layers)
        blob = layer_blob(source, len(layers) - 1)
        blob.write_bytes(blob.read_bytes() + b"\0" * 4096)
        assert_refused_content(run_import(source, store, ok=False))
        assert_unpublished(store)
    # A blob replaced by a symbolic link is content, never a host failure:
    # the copy opens every component with O_NOFOLLOW.
    source, store = base / "symlink-blob", base / "symlink-blob-store"
    layout(source, [raw])
    blob = layer_blob(source)
    elsewhere = source / "elsewhere"
    elsewhere.write_bytes(blob.read_bytes())
    blob.unlink()
    blob.symlink_to(elsewhere)
    assert_refused_content(run_import(source, store, ok=False))
    assert_unpublished(store)
    for name, layers in (("empty", []), ("repeated", [raw, raw])):
        source, store = base / name, base / (name + "-store")
        layout(source, layers)
        imported_artifact(run_import(source, store))
        assert run_helper("verify", "--store", str(store)).returncode == 0


def replace_document(path, value):
    path.chmod(0o600)
    path.write_bytes(canonical(value) + b"\n")
    path.chmod(0o400)


def assert_gc_blocked(store, blobs):
    for command in (("verify",), ("gc",), ("gc", "--apply")):
        result = run_helper(*command, "--store", str(store))
        assert result.returncode == 2, result.stderr
        data = json.loads(result.stdout)["data"]
        assert data["valid"] is False and data["errors"]
        if command[0] == "gc":
            assert data["changed"] is False and data["retired"] == 0
            assert not [c for c in data["candidates"] if c["kind"] == "blob"]
        assert {p.name: digest(p.read_bytes())
                for p in (store / "blobs/sha256").iterdir()} == blobs


def closures(base):
    source, store = base / "closure-layout", base / "closure-store"
    layout(source, [tar_bytes([("one", "file", b"1")]),
                    tar_bytes([("two", "file", b"2")])])
    artifact = imported_artifact(run_import(source, store))
    path = next(store.glob("sources/*/*/closure.json"))
    original = json.loads(path.read_bytes())
    blobs = {p.name: digest(p.read_bytes()) for p in (store / "blobs/sha256").iterdir()}
    changes = [
        lambda c: c.update(layers=[]),
        lambda c: c["layers"].pop(),
        lambda c: c["layers"].reverse(),
        lambda c: c["layers"][0].update(size=c["layers"][0]["size"] + 1),
        lambda c: c["layers"][0].update(mediaType=LAYER + "+gzip"),
        lambda c: c["manifest"].update(digest=c["config"]["digest"]),
        lambda c: c["config"].update(digest=c["layers"][0]["digest"]),
        lambda c: c["manifest"].update(size=c["manifest"]["size"] + 1),
    ]
    for change in changes:
        modified = copy.deepcopy(original)
        change(modified)
        replace_document(path, modified)
        assert_gc_blocked(store, blobs)
        replace_document(path, original)
    metadata_path = artifact / "artifact.json"
    metadata = json.loads(metadata_path.read_bytes())
    for key, value in (("layerDigests", []), ("configDigest", digest(b"other")),
                       ("manifestDigest", digest(b"other"))):
        replace_document(metadata_path, dict(metadata, **{key: value}))
        assert_gc_blocked(store, blobs)
        replace_document(metadata_path, metadata)
    seal_path = artifact / "artifact.seal"
    seal = seal_path.read_bytes()
    seal_path.chmod(0o600)
    seal_path.write_bytes(seal.replace(b"MWOCI/7", b"MWOCI/6")
                         .replace(b"materializer/9", b"materializer/8"))
    seal_path.chmod(0o400)
    assert run_helper("verify", "--store", str(store)).returncode == 2
    retry = run_helper("import", str(source), "--store", str(store), "--apply")
    assert retry.returncode == 1  # Older output is never reused or overwritten.
    seal_path.chmod(0o600)
    seal_path.write_bytes(seal)
    seal_path.chmod(0o400)
    assert run_helper("verify", "--store", str(store)).returncode == 0
    former_artifact(source, store, artifact, metadata, seal)


def former_artifact(source, store, artifact, metadata, seal):
    """A store holding a former-schema artifact is refused by every command,
    each naming the cut; there is no migration, the store is recreated."""
    metadata_path, seal_path = artifact / "artifact.json", artifact / "artifact.seal"
    replace_document(metadata_path, dict(metadata, schema="maelys.oci-artifact/v6"))
    seal_path.chmod(0o600)
    seal_path.write_bytes(seal.replace(b"MWOCI/7", b"MWOCI/6")
                         .replace(b"materializer/9", b"materializer/8"))
    seal_path.chmod(0o400)
    reference = "oci@" + metadata["manifestDigest"]
    cut = "former schema maelys.oci-artifact/v6, which this release does not migrate"
    for arguments in (("list",), ("remove", reference, "--platform", metadata["platform"]),
                      ("remove", reference, "--platform", metadata["platform"], "--apply"),
                      ("import", str(source), "--apply")):
        process = run_helper(*arguments, "--store", str(store))
        assert process.returncode == 1, (arguments, process.stderr)
        error = json.loads(process.stderr)["error"]
        assert error["code"] == "PRECONDITION_FAILED" and cut in error["message"], error
    assert artifact.exists()
    for command in ("verify", "gc"):
        process = run_helper(command, "--store", str(store))
        assert process.returncode == 2, process.stderr
        data = json.loads(process.stdout)["data"]
        assert data["valid"] is False
        assert any(cut in error for error in data["errors"]), data["errors"]
    shutil.rmtree(store)
    imported_artifact(run_import(source, store))
    assert run_helper("verify", "--store", str(store)).returncode == 0


def main():
    assert run_helper("describe", "--summary").returncode == 0
    for command in ("import", "verify", "gc"):
        assert run_helper("describe", command).returncode == 0
    with tempfile.TemporaryDirectory(prefix="maelys-oci-integrity-") as directory:
        base = pathlib.Path(directory)
        whiteouts(base)
        configs_and_encodings(base)
        closures(base)
    print("PASS OCI whiteouts, DiffIDs, encoding, platforms and verified GC roots")


if __name__ == "__main__":
    main()
