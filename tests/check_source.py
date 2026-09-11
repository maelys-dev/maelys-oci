#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Maintained source boundary: short files, no alternate JSON/enterprise seam,
the terminal on the public API alone (docs/policies/layout.md of maelys-platform)."""
from pathlib import Path
import re
import subprocess


def require(condition, message):
    if not condition:
        raise SystemExit(message)

if Path('.git').exists():
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], text=True).split('\0')
    generated = [name for name in tracked if name and
                 (Path(name).parts[0] in ('build', 'dist') or
                  Path(name).parts[0].startswith('build-'))]
    require(not generated, f'generated output is tracked: {generated}')

for root in ('src', 'cli', 'include', 'tests', 'scripts'):
    for path in Path(root).rglob('*'):
        if path.suffix not in ('.c', '.h', '.py', '.sh', '.cpp'):
            continue
        text = path.read_text()
        # The release socle owns the generated dependency-checkout script.
        if path.name == 'checkout-dependency.sh':
            continue
        require(len(text.splitlines()) < 1000, f'{path}: source exceeds 999 lines')
        require('SPDX-License-Identifier: MPL-2.0' in text, f'{path}: missing MPL notice')
        if root == 'src':
            require(not re.search(r'#\s*include\s*[<"]jansson|#\s*if.*ENTERPRISE|OCI_EXTENSION_', text),
                    f'{path}: forbidden source dependency or enterprise seam')
            require(not re.search(r'#\s*include\s*"cli/|#\s*include\s*<maelys/cli\.h>', text),
                    f'{path}: the library does not know the terminal')
        if root == 'cli':
            require(not re.search(r'#\s*include\s*"(src/|\.\./)', text),
                    f'{path}: the terminal includes only the public API')
        if root in ('src', 'cli'):
            # One seam for the HTTP client: acquisition is src/puller's alone.
            # The property holds today; the rule keeps a later widening
            # deliberate and names the offending file before the link step.
            if re.search(r'maelys_http|maelys/http', text):
                require(path.parts[:2] == ('src', 'puller'),
                        f'{path}: only src/puller may reach maelys-http')
require(not Path('src/cli').exists(), 'src/cli: the terminal lives in cli/ at the root')
print('PASS source size, MPL notices, open-core boundary and terminal on the public API')
