#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise the real dependency graph with tiny pinned source repositories."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
MAKE = shutil.which('make')
OUTPUTS = {
    'SYSTEM': ('lib/libmaelys_sys.a',),
    'JSON': ('lib/libmaelys-json.a',),
    'HTTP': ('libmaelys_http.a', 'libmaelys_http_client.a', 'libmaelys_http_tls_mbedtls.a'),
    'CLI': ('lib/libmaelys_cli.a', 'bin/maelys'),
}


class Dependencies(unittest.TestCase):
    def setUp(self):
        (ROOT / 'build').mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='dependency-test-', dir=ROOT / 'build')
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.profile = self.base / 'profile'
        self.env = dict(os.environ)
        for key in list(self.env):
            if key.startswith(('MAKE', 'MFLAGS', 'MAELYS_', 'GIT_')) or key == 'BUILD':
                self.env.pop(key, None)
        self.arguments = [f'BUILD={self.profile}']
        self.sources = {}
        for name, outputs in OUTPUTS.items():
            source = self.base / f'source-{name.lower()}'
            source.mkdir()
            self.sources[name] = source
            (source / 'Makefile').write_text(
                'outputs := ' + ' '.join(f'$(BUILD)/{path}' for path in outputs) + '\n'
                '.PHONY: all check-mbedtls\n'
                'all: $(outputs)\n\t@printf "invoked\\n" >> $(BUILD)/invocations\n'
                'check-mbedtls: all\n'
                '$(outputs):\n\t@mkdir -p $(@D)\n\t@printf "archive\\n" > $@\n')
            self.git(source, 'init', '-q')
            self.git(source, 'add', 'Makefile')
            self.git(source, '-c', 'user.name=Build test', '-c', 'user.email=build@example.invalid',
                     '-c', 'commit.gpgSign=false', 'commit', '-qm', 'fixture')
            pin = self.git(source, 'rev-parse', 'HEAD').strip()
            self.arguments += [f'MAELYS_{name}_DIR={source}', f'MAELYS_{name}_PIN={pin}']

    def git(self, source, *args):
        return subprocess.check_output(['git', '-C', str(source), *args],
                                       env=self.env, text=True, stderr=subprocess.STDOUT)

    def make(self, *args):
        return subprocess.run([MAKE, '--no-print-directory', '-j8', *self.arguments, *args],
                              cwd=ROOT, env=self.env, text=True, capture_output=True)

    def output(self, name, relative):
        return self.profile / 'deps' / f'maelys-{name.lower()}' / relative

    def verify_group(self, name):
        outputs = [self.output(name, path) for path in OUTPUTS[name]]
        consumer = self.profile / 'consumer'
        wrapper = self.base / 'consumer.mk'
        wrapper.write_text(f'{consumer}: ' + ' '.join(map(str, outputs)) + '\n'
                           '\t@cat $^ > $@\n')
        args = ['-f', 'Makefile', '-f', str(wrapper), str(consumer)]
        result = self.make(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        counter = self.output(name, 'invocations')
        self.assertEqual(counter.read_text().splitlines(), ['invoked'])
        unchanged = [p.stat().st_mtime_ns for p in (*outputs, consumer)]
        for _ in range(2):
            result = self.make(*args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual([p.stat().st_mtime_ns for p in (*outputs, consumer)], unchanged)
            self.assertEqual(counter.read_text().splitlines(), ['invoked'])
        for count, output in enumerate(outputs, start=2):
            # GNU make 3.81 on macOS compares whole-second mtimes.
            time.sleep(1.05)
            output.unlink()
            result = self.make(*args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(output.is_file())
            self.assertEqual(len(counter.read_text().splitlines()), count)
            self.assertGreaterEqual(consumer.stat().st_mtime_ns, output.stat().st_mtime_ns)

    def test_cli_parallel_build_incremental_and_missing_outputs(self):
        self.verify_group('CLI')

    def test_http_parallel_build_incremental_and_missing_outputs(self):
        self.verify_group('HTTP')

    def test_rejects_modified_dependency_build_rules(self):
        source = self.sources['JSON'] / 'Makefile'
        source.write_text(source.read_text() + '\n# local modification\n')
        result = self.make('check-dependencies')
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_untracked_dependency_sources(self):
        source = self.sources['JSON'] / 'src'
        source.mkdir()
        (source / 'untracked.c').write_text('/* must not enter a pinned build */\n')
        self.assertNotEqual(self.make('check-dependencies').returncode, 0)


if __name__ == '__main__':
    unittest.main()
