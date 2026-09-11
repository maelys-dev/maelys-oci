#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Adversarial OCI layer/store gates. Names are written into tar bytes, not APFS."""

import bz2
import gzip
import hashlib
import io
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile


HELPER = pathlib.Path(sys.argv[1]).resolve()
SOURCE_ARCHIVE_MAX_MEMBERS = 32768


def digest(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def descriptor(data: bytes, media_type: str, platform=None):
    value = {"mediaType": media_type, "digest": digest(data), "size": len(data)}
    if platform:
        value["platform"] = platform
    return value


def tar_bytes(entries, pax=False) -> bytes:
    output = io.BytesIO()
    fmt = tarfile.PAX_FORMAT if pax else tarfile.USTAR_FORMAT
    with tarfile.open(fileobj=output, mode="w", format=fmt) as archive:
        for entry in entries:
            name, kind = entry[0], entry[1]
            info = tarfile.TarInfo(name)
            info.uid, info.gid, info.mtime = 1234, 2345, 0
            info.mode = entry[3] if len(entry) > 3 else (0o755 if kind == "dir" else 0o644)
            if len(entry) > 4:
                info.pax_headers = entry[4]
            if kind == "dir":
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
            elif kind == "file":
                content = entry[2]
                info.size = len(content)
                archive.addfile(info, io.BytesIO(content))
            elif kind == "symlink":
                info.type = tarfile.SYMTYPE
                info.linkname = entry[2]
                archive.addfile(info)
            elif kind == "hardlink":
                info.type = tarfile.LNKTYPE
                info.linkname = entry[2]
                archive.addfile(info)
            elif kind == "fifo":
                info.type = tarfile.FIFOTYPE
                archive.addfile(info)
            else:
                raise AssertionError(kind)
    return output.getvalue()


def layer_diff_id(layer):
    if not layer.startswith(b"\x1f\x8b"):
        return digest(layer)
    checksum = hashlib.sha256()
    with gzip.GzipFile(fileobj=io.BytesIO(layer)) as stream:
        while block := stream.read(65536):
            checksum.update(block)
    return "sha256:" + checksum.hexdigest()


def layout(root: pathlib.Path, layers, architectures=("arm64",),
           layer_media_type="application/vnd.oci.image.layer.v1.tar") -> list[str]:
    blobs = root / "blobs" / "sha256"
    blobs.mkdir(parents=True)
    layer_descriptors = []
    for layer in layers:
        layer_descriptor = descriptor(layer, layer_media_type)
        (blobs / layer_descriptor["digest"][7:]).write_bytes(layer)
        layer_descriptors.append(layer_descriptor)
    manifests = []
    manifest_digests = []
    for architecture in architectures:
        config = json.dumps({
            "architecture": architecture,
            "os": "linux",
            "config": {"Env": [], "WorkingDir": "/"},
            "rootfs": {"type": "layers", "diff_ids": [layer_diff_id(x) for x in layers]},
        }, sort_keys=True, separators=(",", ":")).encode()
        config_descriptor = descriptor(config, "application/vnd.oci.image.config.v1+json")
        (blobs / config_descriptor["digest"][7:]).write_bytes(config)
        manifest = json.dumps({
            "schemaVersion": 2,
            "mediaType": "application/vnd.oci.image.manifest.v1+json",
            "config": config_descriptor,
            "layers": layer_descriptors,
        }, sort_keys=True, separators=(",", ":")).encode()
        manifest_descriptor = descriptor(
            manifest, "application/vnd.oci.image.manifest.v1+json",
            {"os": "linux", "architecture": architecture})
        (blobs / manifest_descriptor["digest"][7:]).write_bytes(manifest)
        manifests.append(manifest_descriptor)
        manifest_digests.append(manifest_descriptor["digest"])
    (root / "oci-layout").write_text('{"imageLayoutVersion":"1.0.0"}\n')
    (root / "index.json").write_text(json.dumps(
        {"schemaVersion": 2, "manifests": manifests},
        sort_keys=True, separators=(",", ":")) + "\n")
    return manifest_digests


def run_import(source: pathlib.Path, store: pathlib.Path, *extra, ok=True, env=None):
    store.mkdir(mode=0o700)
    process = subprocess.run(
        [str(HELPER), "import", str(source), "--store", str(store), "--apply",
         "--json", "--compact", "--non-interactive", *extra],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    if ok and process.returncode != 0:
        raise AssertionError(process.stderr)
    if not ok and process.returncode == 0:
        raise AssertionError("malicious OCI import unexpectedly succeeded")
    if not ok and list(store.glob(".import.*")):
        raise AssertionError("failed import left a partial staging directory")
    return process


def imported_artifact(process: subprocess.CompletedProcess) -> pathlib.Path:
    """The artifact path comes from the envelope, never from text."""
    envelope = json.loads(process.stdout)
    assert envelope["ok"] is True and envelope["data"]["mode"] == "apply"
    return pathlib.Path(envelope["data"]["artifact"])



def rejected_case(base: pathlib.Path, name: str, entries, pax=False):
    source = base / (name + "-layout")
    layout(source, [tar_bytes(entries, pax=pax)])
    run_import(source, base / (name + "-store"), ok=False)


class ZeroReader(io.RawIOBase):
    def __init__(self, remaining: int):
        self.remaining = remaining

    def readable(self):
        return True

    def readinto(self, buffer):
        amount = min(len(buffer), self.remaining)
        if amount == 0:
            return 0
        buffer[:amount] = bytes(amount)
        self.remaining -= amount
        return amount


def gzip_bomb_layer(size: int) -> bytes:
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz", format=tarfile.USTAR_FORMAT) as archive:
        info = tarfile.TarInfo("zeros")
        info.mode = 0o644
        info.size = size
        archive.addfile(info, ZeroReader(size))
    return output.getvalue()


def archive_layout(source: pathlib.Path, destination: pathlib.Path):
    def normalized(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        info.mtime = 0
        return info

    with tarfile.open(destination, "w", format=tarfile.USTAR_FORMAT) as archive:
        for child in sorted(source.rglob("*")):
            archive.add(child, arcname="./" + child.relative_to(source).as_posix(),
                        recursive=False, filter=normalized)


def corrupt_trailing_header(source: pathlib.Path, destination: pathlib.Path):
    archive_layout(source, destination)
    encoded = bytearray(destination.read_bytes())
    last_content = max(index for index, value in enumerate(encoded) if value)
    end = ((last_content + 1 + 511) // 512) * 512
    assert encoded[end:end + 1024] == bytes(1024)
    encoded[end:end + 512] = b"X" * 512
    destination.write_bytes(encoded)


def duplicate_index_member(source: pathlib.Path, destination: pathlib.Path):
    archive_layout(source, destination)
    content = (source / "index.json").read_bytes()
    with tarfile.open(destination, "a", format=tarfile.USTAR_FORMAT) as archive:
        entry = tarfile.TarInfo("./index.json")
        entry.mode = 0o644
        entry.size = len(content)
        archive.addfile(entry, io.BytesIO(content))


def archive_budget_case(base: pathlib.Path):
    source = base / "member-budget.oci.tar"
    with tarfile.open(source, "w", format=tarfile.USTAR_FORMAT) as archive:
        for index in range(SOURCE_ARCHIVE_MAX_MEMBERS + 1):
            entry = tarfile.TarInfo(f"ignored/{index:05d}")
            entry.mode = 0o644
            entry.size = 0
            archive.addfile(entry)
    result = run_helper("inspect", str(source))
    assert result.returncode != 0
    assert "security budgets" in result.stderr, result.stderr


def run_helper(*arguments):
    return subprocess.run(
        [str(HELPER), *arguments, "--json", "--compact", "--non-interactive"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def verify_errors(store: pathlib.Path) -> list[str]:
    process = run_helper("verify", "--store", str(store))
    assert process.returncode == 2, process.stderr
    data = json.loads(process.stdout)["data"]
    assert data["valid"] is False
    return data["errors"]


def lease_cases(base: pathlib.Path):
    """Lease files are trusted only in their exact v3 shape."""
    source = base / "lease-layout"
    manifests = layout(source, [tar_bytes([("ok", "file", b"x")])])
    store = base / "lease-store"
    artifact = imported_artifact(run_import(source, store))
    metadata = json.loads((artifact / "artifact.json").read_text())
    leases = store / "leases"
    valid = {
        "createdUnixSeconds": 0, "expiresUnixSeconds": 0,
        "liveness": "kernel-lock", "manifestDigest": manifests[0],
        "platform": "linux/arm64", "rootDigest": metadata["rootDigest"],
        "rootfsTarDigest": metadata["rootfsTarDigest"],
        "schema": "maelys.warden.oci-lease/v3",
    }
    lease = leases / ("a" * 32 + ".json")

    def plant(document, mode=0o400):
        if lease.exists():
            lease.chmod(0o600)
            lease.unlink()
        lease.write_text(json.dumps(document, separators=(",", ":")) + "\n")
        lease.chmod(mode)

    plant(valid)
    process = run_helper("verify", "--store", str(store))
    assert process.returncode == 0, process.stderr
    warnings = json.loads(process.stdout)["data"]["warnings"]
    assert any("no live holder" in warning for warning in warnings), warnings

    for name, document in (
        ("process-liveness", {**valid, "liveness": "process"}),
        ("v2-schema", {**valid, "schema": "maelys.warden.oci-lease/v2"}),
        ("owner-fields", {**valid, "ownerPid": os.getpid(), "ownerStart": "x"}),
        ("missing-liveness", {key: value for key, value in valid.items()
                              if key != "liveness"}),
        ("expiry-before-creation", {**valid, "createdUnixSeconds": 5}),
        ("foreign-platform", {**valid, "platform": "Linux/arm64"}),
        ("bad-digest", {**valid, "rootDigest": "sha256:zz"}),
    ):
        plant(document)
        errors = verify_errors(store)
        assert any("malformed" in error for error in errors), (name, errors)
        process = run_helper("gc", "--store", str(store), "--grace-seconds", "0")
        assert process.returncode == 2, (name, process.stderr)
        assert lease.exists(), name

    plant({**valid, "manifestDigest": "sha256:" + "0" * 64})
    errors = verify_errors(store)
    assert any("absent artifact" in error for error in errors), errors

    plant(valid, mode=0o600)
    process = run_helper("gc", "--store", str(store), "--grace-seconds", "0")
    assert process.returncode == 2, process.stderr
    assert lease.exists()

    lease.chmod(0o600)
    lease.unlink()
    alias = leases / ("b" * 32 + ".json")
    alias.symlink_to(artifact / "artifact.json")
    errors = verify_errors(store)
    assert any("invalid OCI lease entry" in error or "malformed" in error
               for error in errors), errors
    alias.unlink()
    process = run_helper("verify", "--store", str(store))
    assert process.returncode == 0, process.stderr


def main():
    with tempfile.TemporaryDirectory(prefix="maelys-oci-adversarial.") as temporary:
        base = pathlib.Path(temporary)

        no_external_decompressors(base)

        rejected_case(base, "traversal", [("../escape", "file", b"x")])
        rejected_case(base, "absolute", [("/escape", "file", b"x")])
        rejected_case(base, "symlink-pivot", [
            ("pivot", "symlink", "/"), ("pivot/etc/passwd", "file", b"x")])
        rejected_case(base, "hardlink-escape", [
            ("safe", "file", b"x"), ("alias", "hardlink", "../safe")])
        rejected_case(base, "malformed-whiteout", [(".wh.", "file", b"")])
        rejected_case(base, "special-fifo", [("channel", "fifo", b"")])
        rejected_case(base, "unsupported-xattr", [
            ("xattr", "file", b"x", 0o644,
             {"SCHILY.xattr.user.test": "eA=="})], pax=True)
        rejected_case(base, "unsupported-posix-acl", [
            ("acl", "file", b"secret", 0o770,
             {"SCHILY.acl.access":
              "user::rwx,group::---,user:1001:rwx,mask::rwx,other::---"})],
            pax=True)

        charset_source = base / "safe-pax-charset-layout"
        layout(charset_source, [tar_bytes([
            ("binary-name", "file", b"ok", 0o644,
             {"hdrcharset": "BINARY"})], pax=True)])
        run_import(charset_source, base / "safe-pax-charset-store")

        bomb = base / "decompression-bomb-layout"
        layout(bomb, [gzip_bomb_layer(96 * 1024 * 1024)],
               layer_media_type="application/vnd.oci.image.layer.v1.tar+gzip")
        run_import(bomb, base / "decompression-bomb-store", ok=False)

        unsupported = base / "unsupported-media-layout"
        layout(unsupported, [tar_bytes([("ok", "file", b"x")])],
               layer_media_type="application/example.unsupported")
        run_import(unsupported, base / "unsupported-media-store", ok=False)

        # A valid tar inside an unadvertised compression format must not
        # activate libarchive's broad decoder/plugin/external-helper registry.
        unsupported_compression = base / "unsupported-compression-layout"
        layout(unsupported_compression,
               [bz2.compress(tar_bytes([("ok", "file", b"x")]))])
        run_import(unsupported_compression, base / "unsupported-compression-store", ok=False)

        archive_path = base / "layout.tar.bz2"
        with tarfile.open(archive_path, "w:bz2") as archive:
            archive.add(unsupported_compression, arcname=".")
        refused = run_helper("inspect", str(archive_path))
        assert refused.returncode != 0, "unsupported source compression was accepted"

        malformed_source = base / "malformed-source-layout"
        layout(malformed_source, [tar_bytes([("ok", "file", b"x")])])
        malformed_archive = base / "malformed-trailing-header.oci.tar"
        corrupt_trailing_header(malformed_source, malformed_archive)
        inspected = run_helper("inspect", str(malformed_archive))
        assert inspected.returncode != 0, "malformed archive was inspected"
        run_import(malformed_archive, base / "malformed-archive-store", ok=False)

        duplicate_archive = base / "duplicate-index.oci.tar"
        duplicate_index_member(malformed_source, duplicate_archive)
        duplicated = run_helper("inspect", str(duplicate_archive))
        assert duplicated.returncode != 0
        assert "duplicate OCI member" in duplicated.stderr, duplicated.stderr

        archive_budget_case(base)

        source = base / "whiteout-layout"
        first = tar_bytes([("gone", "file", b"old"), ("keep", "file", b"yes")])
        second = tar_bytes([(".wh.gone", "file", b"")])
        digests = layout(source, [first, second])
        result = run_import(source, base / "whiteout-store", "--platform", "linux/arm64")
        artifact = imported_artifact(result)
        metadata = json.loads((artifact / "artifact.json").read_text())
        assert metadata["manifestDigest"] == digests[0]

        churn = base / "graph-churn-layout"
        churn_entries = [
            ("hot", "file", f"revision-{index}".encode())
            for index in range(1500)
        ]
        layout(churn, [tar_bytes(churn_entries)])
        result = run_import(churn, base / "graph-churn-store")
        artifact = imported_artifact(result)
        metadata = json.loads((artifact / "artifact.json").read_text())
        # Repeated writes leave only the image's final file, with no helpers.
        assert metadata["logicalEntries"] == 1
        assert not (artifact / "content").exists()
        assert not (artifact / "descriptors").exists()

        privileged_source = base / "privileged-layout"
        layout(privileged_source, [tar_bytes([("tool", "file", b"ok", 0o6755)])])
        result = run_import(privileged_source, base / "privileged-store")
        artifact = imported_artifact(result)
        metadata = json.loads((artifact / "artifact.json").read_text())
        assert metadata["transformations"]["clearedSetuidSetgid"] == 1
        with (artifact / "root.ext4").open("rb") as image:
            image.seek(-1024 * 1024, os.SEEK_END)
            assert image.read() == bytes(1024 * 1024), "unused tail blocks are not zero"

        ambiguous = base / "ambiguous-layout"
        manifests = layout(ambiguous, [tar_bytes([("ok", "file", b"x")])],
                           architectures=("arm64", "amd64"))
        run_import(ambiguous, base / "ambiguous-store", ok=False)
        selected = run_import(ambiguous, base / "selected-store",
                              "--digest", manifests[1])
        assert json.loads(selected.stdout)["data"]["platform"] == "linux/amd64"

        tampered = base / "tampered-layout"
        layout(tampered, [tar_bytes([("ok", "file", b"x")])])
        blob = next((tampered / "blobs" / "sha256").iterdir())
        blob.write_bytes(blob.read_bytes() + b"tamper")
        run_import(tampered, base / "tampered-store", ok=False)

        lease_cases(base)

    print("PASS adversarial OCI paths, media, expansion, metadata and atomic store gates")


def no_external_decompressors(base):
    # grzip's file signature selects a decoder that libarchive implements
    # solely by executing `grzip -d`. This harmless fixture helper records
    # an invocation; rejecting the bad archive alone would miss that effect.
    programs = base / 'programs'
    programs.mkdir()
    marker = base / 'external-decompressor-ran'
    helper = programs / 'grzip'
    helper.write_text('#!/bin/sh\n: > ' + shlex.quote(str(marker)) + '\nexit 1\n')
    helper.chmod(0o700)
    environment = dict(os.environ, PATH=str(programs))
    encoded = bytes.fromhex('47525a697049490002043a29') + bytes(1024)
    source = base / 'external-decoder-layout'
    layout(source, [encoded])
    run_import(source, base / 'external-decoder-store', ok=False, env=environment)
    assert not marker.exists(), 'layer decoding executed an external program'
    archive = base / 'external-decoder.oci.tar'
    archive.write_bytes(encoded)
    result = subprocess.run([str(HELPER), 'inspect', str(archive),
                             '--format', 'json', '--non-interactive'],
                            env=environment, capture_output=True, text=True)
    assert result.returncode != 0
    assert not marker.exists(), 'source decoding executed an external program'


if __name__ == "__main__":
    main()
