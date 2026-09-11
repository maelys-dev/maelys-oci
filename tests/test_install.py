#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Change PREFIX without cleaning, then verify the actual installed artifacts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    build = Path(sys.argv[1]).resolve()
    binary = build / 'bin/maelys-oci'
    binary_mtime = binary.stat().st_mtime_ns
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    env = dict(os.environ)
    for name in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'):
        env.pop(name, None)
    make = [shutil.which('make'), '--no-print-directory', f'BUILD={build}']
    pc = build / 'lib/pkgconfig/maelys-oci.pc'
    manifest = build / 'share/maelys/commands/oci.json'
    original_prefix = pc.read_text().splitlines()[0].removeprefix('prefix=')

    def invoke(*args):
        result = subprocess.run([*make, *args], cwd=ROOT, env=env,
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    try:
        with tempfile.TemporaryDirectory(prefix='install-test-', dir=build) as temporary:
            for prefix in ('/usr/local', '/opt/maelys-audit'):
                invoke('install', f'PREFIX={prefix}', f'DESTDIR={temporary}')
                stage = Path(temporary) / prefix.lstrip('/')
                installed = stage / 'bin/maelys-oci'
                data = json.loads((stage / 'share/maelys/commands/oci.json').read_text())
                assert data['executable'] == f'{prefix}/bin/maelys-oci'
                installed_digest = hashlib.sha256(installed.read_bytes()).hexdigest()
                assert data['sha256'] == digest == installed_digest, (
                    f"manifest {data['sha256']}, build {digest}, installed {installed_digest}")
                assert (stage / 'lib/pkgconfig/maelys-oci.pc').read_text().splitlines()[0] == f'prefix={prefix}'
                assert (stage / 'include/maelys/oci.h').read_bytes() == (ROOT / 'include/maelys/oci.h').read_bytes()
                assert installed.stat().st_mode & 0o777 == 0o755
                for relative in ('lib/libmaelys-oci.a', 'lib/pkgconfig/maelys-oci.pc',
                                 'share/maelys/commands/oci.json',
                                 'share/bash-completion/completions/maelys-oci',
                                 'share/zsh/site-functions/_maelys-oci',
                                 'share/fish/vendor_completions.d/maelys-oci.fish'):
                    assert (stage / relative).stat().st_mode & 0o777 == 0o644, relative
                mtimes = [p.stat().st_mtime_ns for p in (pc, manifest)]
                invoke('all', f'PREFIX={prefix}')
                assert [p.stat().st_mtime_ns for p in (pc, manifest)] == mtimes
                assert binary.stat().st_mtime_ns == binary_mtime, 'unchanged binary was relinked'
    finally:
        invoke('install-metadata', f'PREFIX={original_prefix}')
    print('PASS installation, changed prefix, digest binding, modes and incremental build')


if __name__ == '__main__':
    main()
