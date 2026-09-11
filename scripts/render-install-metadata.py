#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Render installation metadata without stale Make-variable substitutions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def publish(path, content):
    """Replace a generated build file atomically; preserve unchanged mtimes."""
    path = Path(path)
    content = content.encode('utf-8')
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f'.{path.name}.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(content)
            os.fchmod(output.fileno(), 0o644)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('prefix', 'version', 'binary', 'pkgconfig', 'manifest',
                 'http-libs', 'private-libs'):
        parser.add_argument(f'--{name}', required=True)
    args = parser.parse_args()
    if not Path(args.prefix).is_absolute() or any(c in args.prefix for c in '\n\r\0'):
        parser.error('prefix must be an absolute, single-line path')
    pc = (ROOT / 'packaging/maelys-oci.pc.in').read_text()
    for name, value in (('PREFIX', args.prefix), ('VERSION', args.version),
                        ('HTTP_LIBS', args.http_libs),
                        ('PLATFORM_PRIVATE_LIBS', args.private_libs)):
        pc = pc.replace(f'@{name}@', value)
    manifest = json.loads((ROOT / 'cli/command.json.in').read_text())
    manifest['executable'] = str(Path(args.prefix) / 'bin/maelys-oci')
    manifest['version'] = args.version
    with open(args.binary, 'rb') as binary:
        digest = hashlib.sha256()
        for block in iter(lambda: binary.read(65536), b''):
            digest.update(block)
    manifest['sha256'] = digest.hexdigest()
    publish(args.pkgconfig, pc)
    publish(args.manifest, json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
