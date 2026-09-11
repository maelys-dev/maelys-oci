#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Build placement, cleanup isolation and generated-file tracking regressions."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
MAKE = shutil.which('make')


class BuildLayout(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='layout-test-', dir=ROOT / 'build')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.env = dict(os.environ)
        for key in ('BUILD', 'BUILD_ROOT', 'BUILD_PLATFORM', 'MAKEFLAGS', 'MFLAGS',
                    'MAKELEVEL', 'MAELYS_SYSTEM_BUILD', 'MAELYS_JSON_BUILD',
                    'MAELYS_HTTP_BUILD', 'MAELYS_CLI_BUILD'):
            self.env.pop(key, None)

    def make(self, *args):
        return subprocess.run([MAKE, '--no-print-directory', *args], cwd=ROOT,
                              env=self.env, capture_output=True, text=True)

    def test_default_profile(self):
        platform = subprocess.check_output([ROOT / 'scripts/release-target.sh'], text=True).strip()
        result = self.make('-n', 'clean')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(ROOT / 'build' / platform / 'release'), result.stdout)

    def test_clean_preserves_other_profiles(self):
        selected = self.base / 'release'
        sibling = self.base / 'sanitizers'
        for directory in (selected, sibling):
            directory.mkdir()
            (directory / 'sentinel').write_text('keep until this profile is cleaned')
        result = self.make('clean', f'BUILD={selected.relative_to(ROOT)}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(selected.exists())
        self.assertTrue((sibling / 'sentinel').is_file())
        result = self.make('clean', f'BUILD={sibling}')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(sibling.exists())

    def test_refuses_unsafe_output_roots(self):
        for directory in ('', '.', 'build', 'build/../src', 'build-amd64', str(ROOT.parent)):
            with self.subTest(directory=directory):
                result = self.make('clean', f'BUILD={directory}')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('build directory', result.stderr)

    def test_refuses_symlink_escape(self):
        link = self.base / 'outside'
        link.symlink_to(ROOT / 'src', target_is_directory=True)
        result = self.make('clean', f'BUILD={link / "generated"}')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('build directory', result.stderr)
        self.assertTrue((ROOT / 'src/common').is_dir())

    def test_dependency_output_stays_in_build(self):
        for dependency in ('SYSTEM', 'JSON', 'HTTP', 'CLI'):
            with self.subTest(dependency=dependency):
                result = self.make('-n', 'clean', f'BUILD={self.base / "release"}',
                                   f'MAELYS_{dependency}_BUILD={ROOT / "src"}')
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('build directory', result.stderr)

    @unittest.skipUnless((ROOT / '.git').exists(), 'source archive has no Git index')
    def test_source_check_refuses_tracked_output(self):
        # Exercise a forced addition in an isolated index; leave the user's
        # index and worktree untouched.
        index = subprocess.check_output(['git', 'rev-parse', '--git-path', 'index'],
                                        cwd=ROOT, text=True).strip()
        isolated = self.base / 'index'
        shutil.copyfile(ROOT / index, isolated)
        self.env['GIT_INDEX_FILE'] = str(isolated)
        blob = subprocess.check_output(['git', 'rev-parse', 'HEAD:VERSION'],
                                       cwd=ROOT, text=True).strip()
        for name in ('build/generated.c', 'build-amd64/generated.c', 'dist/package.tar.gz'):
            subprocess.run(['git', 'update-index', '--add', '--cacheinfo',
                            f'100644,{blob},{name}'], cwd=ROOT, env=self.env, check=True)
        result = subprocess.run([sys.executable, '-O', 'tests/check_source.py'], cwd=ROOT,
                                env=self.env, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('generated output is tracked', result.stderr)


if __name__ == '__main__':
    unittest.main()
