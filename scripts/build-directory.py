#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Resolve one build directory beneath this repository's build/ directory."""
from pathlib import Path
import sys


def main():
    root = Path(__file__).resolve().parent.parent / 'build'
    if len(sys.argv) != 2 or not sys.argv[1].strip():
        sys.exit('a nonempty build directory is required')
    try:
        directory = Path(sys.argv[1]).resolve()
        relative = directory.relative_to(root)
    except (ValueError, OSError, RuntimeError):
        sys.exit(f'build directory must be beneath {root}')
    if not relative.parts or any(c.isspace() for c in str(directory)):
        sys.exit(f'build directory must be a subdirectory of {root}, without whitespace')
    print(directory)


if __name__ == '__main__':
    main()
