#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Image metadata and local index traversal through the public terminal."""
import copy
import gzip
import hashlib
import io
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

from schema_assert import assert_schema

OCI = pathlib.Path(sys.argv[1]).resolve()
STAT_SCHEMA = json.loads((pathlib.Path(__file__).resolve().parents[1] / "cli/schemas/stat.json").read_text())
INDEX = "application/vnd.oci.image.index.v1+json"
MANIFEST = "application/vnd.oci.image.manifest.v1+json"
CONFIG = "application/vnd.oci.image.config.v1+json"
LAYER = "application/vnd.oci.image.layer.v1.tar"


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()


def put_blob(root, data, media_type):
    identifier = digest(data)
    directory = root / "blobs" / "sha256"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / identifier[7:]).write_bytes(data)
    return {"mediaType": media_type, "digest": identifier, "size": len(data)}


def layer(name, content):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        entry = tarfile.TarInfo(name)
        entry.mode = 0o644
        entry.size = len(content)
        archive.addfile(entry, io.BytesIO(content))
    return output.getvalue()


def image(root, architecture="arm64", changes=None):
    raw_layers = [layer("hello", b"hello\n"), layer("world", b"world\n")]
    layers = [put_blob(root, gzip.compress(raw_layers[0], mtime=0), LAYER + "+gzip"),
              put_blob(root, raw_layers[1], LAYER)]
    config = {
        "architecture": architecture, "os": "linux",
        "created": "2026-09-29T00:00:00Z", "author": "fixture author",
        "config": {"User": "1000:1000", "Env": ["PORT=8080", "EMPTY="],
                   "Entrypoint": ["python"], "Cmd": ["app.py", "a\"b"],
                   "WorkingDir": "/app", "Labels": {"title": "étiquette\nquoted \"text\""},
                   "ExposedPorts": {"8080/tcp": {}}, "Volumes": {"/data": {}},
                   "StopSignal": "SIGTERM", "ArgsEscaped": False},
        "rootfs": {"type": "layers", "diff_ids": [digest(x) for x in raw_layers]},
        "history": [{"created_by": "ENV PORT=8080", "empty_layer": True},
                    {"created_by": "ADD hello", "comment": "first layer"},
                    {"created_by": "ADD world", "author": "builder"}],
        "unknown-extension": {"a-number": 1.25}
    }
    if changes:
        config.update(changes)
    config_descriptor = put_blob(root, encoded(config), CONFIG)
    manifest = {"schemaVersion": 2, "mediaType": MANIFEST,
                "config": config_descriptor, "layers": layers,
                "annotations": {"org.opencontainers.image.title": "example"}}
    return put_blob(root, encoded(manifest), MANIFEST), config, layers


def index(root, children, **changes):
    value = {"schemaVersion": 2, "mediaType": INDEX, "manifests": children}
    value.update(changes)
    return put_blob(root, encoded(value), INDEX)


def layout(root, children):
    (root / "oci-layout").write_bytes(encoded({"imageLayoutVersion": "1.0.0"}))
    (root / "index.json").write_bytes(encoded({"schemaVersion": 2, "manifests": children}))


def run(command, *arguments, code=0):
    result = subprocess.run([str(OCI), command, *map(str, arguments),
                             "--format", "json", "--compact", "--non-interactive"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert result.returncode == code, (command, result.returncode,
                                       result.stdout[:2048], result.stderr[:2048])
    envelope = json.loads(result.stdout if code == 0 else result.stderr)
    assert envelope["ok"] == (code == 0), envelope
    assert not (result.stderr if code == 0 else result.stdout), result
    if code == 0 and command == "stat":
        assert_schema(envelope["data"], STAT_SCHEMA)
    return envelope["data"] if code == 0 else envelope["error"]


def snapshot(root):
    return {str(p.relative_to(root)): (p.stat().st_mode, digest(p.read_bytes()))
            for p in root.rglob("*") if p.is_file()}


def metadata_case(base):
    root = base / "metadata"
    manifest, config, layers = image(root)
    layout(root, [manifest])
    before = snapshot(root)
    compact = run("inspect", root)["manifests"][0]
    report = run("stat", root)
    assert report["schema"] == "maelys.oci-stat/v1"
    detail = report["manifests"][0]
    assert set(compact) == {"digest", "platform", "configDigest", "compressedLayerBytes", "layerCount"}
    assert all(detail[k] == v for k, v in compact.items())
    assert detail["config"] == config["config"]
    assert detail["author"] == config["author"] and detail["created"] == config["created"]
    assert detail["annotations"] == {"org.opencontainers.image.title": "example"}
    assert detail["manifestBytes"] == manifest["size"]
    assert len(detail["layers"]) == 2
    for i, item in enumerate(detail["layers"]):
        assert item == dict(layers[i], diffId=config["rootfs"]["diff_ids"][i])
    assert layers[0]["digest"] != detail["layers"][0]["diffId"]
    history = detail["history"]
    assert history[0] == {"emptyLayer": True, "createdBy": "ENV PORT=8080"}
    assert [h["layerIndex"] for h in history[1:]] == [0, 1]
    assert [h["layerDigest"] for h in history[1:]] == [x["digest"] for x in layers]
    assert snapshot(root) == before
    # These are declared layer descriptors, not a claim of reading layer bytes.
    for item in layers:
        (root / "blobs" / "sha256" / item["digest"][7:]).unlink()
    assert run("stat", root)["manifests"] == report["manifests"]
    print("PASS detailed metadata, empty history records, escaping and read-only behavior")


def optional_metadata_case(base):
    for number, changes in enumerate([
        {"config": None, "history": None, "author": None, "created": None},
        {"config": {"User": "", "Cmd": [], "Env": None, "unknown": 1.25}, "history": []}
    ]):
        root = base / f"optional-{number}"
        manifest, _, _ = image(root, changes=changes)
        layout(root, [manifest])
        detail = run("stat", root)["manifests"][0]
        assert detail["history"] == []
        assert detail["config"] == ({} if number == 0 else {"User": "", "Cmd": []})
        if number == 0:
            assert "created" not in detail and "author" not in detail
    for number, changes in enumerate([
        {"config": {"Env": "not an array"}},
        {"config": {"Labels": {"title": 1}}},
        {"config": {"User": "bad\0name"}},
        {"history": [{}, {}, {}]},
        {"history": [{"empty_layer": "false"}]}
    ]):
        root = base / f"invalid-metadata-{number}"
        manifest, _, _ = image(root, changes=changes)
        layout(root, [manifest])
        assert run("stat", root, code=1)["code"] == "PROTOCOL_FAILED"
    print("PASS optional/null metadata, unknown extensions and malformed history")


def nested_case(base):
    root = base / "nested"
    arm, _, _ = image(root, "arm64")
    intel, _, _ = image(root, "amd64")
    # No leaf platform descriptors: the config supplies each actual platform.
    child = index(root, [arm, intel])
    layout(root, [index(root, [child])])
    before = snapshot(root)
    manifests = run("inspect", root)["manifests"]
    assert {m["platform"] for m in manifests} == {"linux/arm64", "linux/amd64"}
    assert {m["digest"] for m in manifests} == {arm["digest"], intel["digest"]}
    assert len(run("stat", root)["manifests"]) == 2
    store = base / "nested-store"
    assert run("import", root, "--store", store, code=1)["code"] == "PRECONDITION_FAILED"
    plan = run("import", root, "--store", store, "--platform", "linux/arm64")
    assert plan["manifestDigest"] == arm["digest"] and plan["mode"] == "plan"
    assert not store.exists() and snapshot(root) == before
    applied = run("import", root, "--store", store, "--platform", "linux/arm64", "--apply")
    assert applied["mode"] == "apply" and applied["manifestDigest"] == arm["digest"]
    assert run("verify", "--store", store)["valid"] is True
    flat = base / "flat-equivalent"
    shutil.copytree(root, flat)
    layout(flat, [arm, intel])
    flat_applied = run("import", flat, "--store", base / "flat-store",
                       "--platform", "linux/arm64", "--apply")
    for name in ["root.ext4", "rootfs.tar"]:
        checksums = []
        for result in [applied, flat_applied]:
            checksum = hashlib.sha256()
            with (pathlib.Path(result["artifact"]) / name).open("rb") as source:
                for block in iter(lambda: source.read(65536), b""):
                    checksum.update(block)
            checksums.append(checksum.hexdigest())
        assert checksums[0] == checksums[1], (name, checksums)
    archive = base / "nested.tar"
    def portable_member(entry):
        entry.uid = entry.gid = entry.mtime = 0
        entry.uname = entry.gname = ""
        return entry
    with tarfile.open(archive, "w", format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(root.rglob("*")):
            tar.add(path, arcname=str(path.relative_to(root)), recursive=False,
                    filter=portable_member)
    assert run("inspect", archive)["manifests"] == manifests
    assert run("stat", archive)["manifests"] == run("stat", root)["manifests"]
    assert run("import", archive, "--store", base / "archive-store", "--digest", arm["digest"])["mode"] == "plan"
    print("PASS nested multi-platform directory/archive, selection, plan/apply and store verification")


def repeated_case(base):
    root = base / "repeated"
    manifest, _, _ = image(root)
    child = index(root, [manifest])
    layout(root, [child, child, manifest])
    assert len(run("inspect", root)["manifests"]) == 1
    assert len(run("stat", root)["manifests"]) == 1
    assert run("import", root, "--store", base / "repeat-plan")["mode"] == "plan"
    print("PASS repeated references do not make one image ambiguous")


def invalid_index_case(base):
    for name in ["digest", "size", "schema", "media", "platform", "parent-platform", "missing"]:
        root = base / f"invalid-index-{name}"
        manifest, _, _ = image(root, "amd64" if name == "parent-platform" else "arm64")
        if name == "platform":
            manifest["platform"] = {"os": "linux", "architecture": "amd64"}
        changes = {"schemaVersion": 1} if name == "schema" else {}
        if name == "media":
            changes["mediaType"] = MANIFEST
        child = index(root, [manifest], **changes)
        if name in ["platform", "parent-platform"]:
            child["platform"] = {"os": "linux", "architecture": "arm64"}
        path = root / "blobs" / "sha256" / child["digest"][7:]
        if name == "digest":
            path.write_bytes(path.read_bytes().replace(b'"schemaVersion":2', b'"schemaVersion":3'))
        elif name == "size":
            child["size"] += 1
        elif name == "missing":
            path.unlink()
        layout(root, [child])
        store = base / f"refused-{name}"
        for command in ["inspect", "stat", "import"]:
            options = ["--store", store] if command == "import" else []
            assert run(command, root, *options, code=1)["code"] == "PROTOCOL_FAILED"
        assert not store.exists()
    print("PASS corrupt/missing indexes, embedded media type and inherited platform constraints")


def bounds_case(base):
    for depth in [8, 9]:
        root = base / f"depth-{depth}"
        descriptor, _, _ = image(root)
        for _ in range(depth):
            descriptor = index(root, [descriptor])
        layout(root, [descriptor])
        if depth == 8:
            assert len(run("inspect", root)["manifests"]) == 1
        else:
            assert run("inspect", root, code=1)["code"] == "PROTOCOL_FAILED"
    root = base / "descriptor-limit"
    manifest, _, _ = image(root)
    layout(root, [index(root, [manifest])] * 1024)
    error = run("inspect", root, code=1)
    assert error["code"] == "PROTOCOL_FAILED" and "complete traversal" in error["message"]
    root = base / "unsupported-sibling"
    manifest, _, _ = image(root)
    foreign = copy.deepcopy(manifest)
    foreign["mediaType"] = INDEX
    foreign["platform"] = {"os": "windows", "architecture": "amd64"}
    layout(root, [index(root, [manifest, foreign])])
    assert len(run("inspect", root)["manifests"]) == 1
    print("PASS depth/descriptor ceilings and unsupported platform siblings")


def metadata_limit_case(base):
    root = base / "metadata-limit"
    manifests = []
    for number in range(2):
        manifest, _, _ = image(root, changes={
            "author": f"builder-{number}",
            "config": {"Labels": {"large": "x" * (4 * 1024 * 1024)}}
        })
        manifests.append(manifest)
    layout(root, manifests)
    assert len(run("inspect", root)["manifests"]) == 2
    error = run("stat", root, code=1)
    assert error["code"] == "PROTOCOL_FAILED" and "aggregate" in error["message"]
    root = base / "history-limit"
    manifests = []
    for number in range(2):
        manifest, _, _ = image(root, changes={
            "author": f"builder-{number}",
            "history": [{"empty_layer": True}] * 9000
        })
        manifests.append(manifest)
        layout(root, [manifest])
        assert len(run("stat", root)["manifests"]) == 1
    layout(root, manifests)
    error = run("stat", root, code=1)
    assert error["code"] == "PROTOCOL_FAILED" and "token limit" in error["message"]
    print("PASS aggregate metadata bytes and rendered report token ceilings")


def main():
    summary = run("describe", "--summary")
    assert "stat" in {c["id"] for c in summary["commands"]}
    for command in ["inspect", "stat", "import", "verify"]:
        descriptor = run("describe", command)["commands"][0]
        assert descriptor["id"] == command and "outputSchema" in descriptor
    descriptor = run("describe", "stat")["commands"][0]
    assert descriptor["effect"] == "read" and descriptor["input"]["options"] == []
    assert descriptor["outputSchema"]["properties"]["schema"]["const"] == "maelys.oci-stat/v1"
    assert run("stat", code=1)["code"] == "VALIDATION_FAILED"
    assert run("stat", "/absent", "--apply", code=1)["code"] == "VALIDATION_FAILED"
    with tempfile.TemporaryDirectory(prefix="maelys-oci-inspection-") as directory:
        base = pathlib.Path(directory)
        metadata_case(base)
        optional_metadata_case(base)
        nested_case(base)
        repeated_case(base)
        invalid_index_case(base)
        bounds_case(base)
        metadata_limit_case(base)


if __name__ == "__main__":
    main()
