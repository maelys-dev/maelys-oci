#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Build a minimal OCI layout around one already-created Linux layer tar."""

import hashlib
import json
import pathlib
import shutil
import sys


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def canonical(document: object) -> bytes:
    return json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")


def store_blob(layout: pathlib.Path, content: bytes) -> tuple[str, int]:
    identity = hashlib.sha256(content).hexdigest()
    target = layout / "blobs" / "sha256" / identity
    target.write_bytes(content)
    return identity, len(content)


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print("usage: make_oci_layout.py LAYER.tar LAYOUT [linux/arm64|linux/amd64]", file=sys.stderr)
        return 64
    layer = pathlib.Path(sys.argv[1]).resolve()
    layout = pathlib.Path(sys.argv[2]).resolve()
    platform = sys.argv[3] if len(sys.argv) == 4 else "linux/arm64"
    if platform not in {"linux/arm64", "linux/amd64"}:
        return 64
    os_name, architecture = platform.split("/", 1)
    if not layer.is_file() or layout.exists():
        return 66
    (layout / "blobs" / "sha256").mkdir(parents=True, mode=0o700)
    layer_digest = digest(layer)
    layer_target = layout / "blobs" / "sha256" / layer_digest
    shutil.copyfile(layer, layer_target)
    config_digest, config_size = store_blob(
        layout,
        canonical(
            {
                "architecture": architecture,
                "config": {
                    "Env": [
                        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
                    ],
                    "WorkingDir": "/",
                },
                "os": os_name,
                "rootfs": {"diff_ids": [f"sha256:{layer_digest}"], "type": "layers"},
            }
        ),
    )
    manifest_digest, manifest_size = store_blob(
        layout,
        canonical(
            {
                "config": {
                    "digest": f"sha256:{config_digest}",
                    "mediaType": "application/vnd.oci.image.config.v1+json",
                    "size": config_size,
                },
                "layers": [
                    {
                        "digest": f"sha256:{layer_digest}",
                        "mediaType": "application/vnd.oci.image.layer.v1.tar",
                        "size": layer.stat().st_size,
                    }
                ],
                "mediaType": "application/vnd.oci.image.manifest.v1+json",
                "schemaVersion": 2,
            }
        ),
    )
    (layout / "oci-layout").write_bytes(canonical({"imageLayoutVersion": "1.0.0"}))
    (layout / "index.json").write_bytes(
        canonical(
            {
                "manifests": [
                    {
                        "digest": f"sha256:{manifest_digest}",
                        "mediaType": "application/vnd.oci.image.manifest.v1+json",
                        "platform": {"architecture": architecture, "os": os_name},
                        "size": manifest_size,
                    }
                ],
                "schemaVersion": 2,
            }
        )
    )
    print(f"sha256:{manifest_digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
