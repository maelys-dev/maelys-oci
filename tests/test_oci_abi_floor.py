#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""MAELYS_OCI_ABI_COMPATIBLE_SINCE is held to the header it names.

tests/public/abi-N.h is <maelys/oci.h> as revision N published it. Every
declaration of that file must stand unchanged in the current header; an
enumeration may only have gained members after its last. A declaration that
moved fails here, and the way out is to raise the floor to the revision that
moves it, freeze that header beside this one and name the break in the
changelog: never to edit the frozen copy.
"""
import pathlib
import re
import sys

HEADER = pathlib.Path(sys.argv[1])
FROZEN = pathlib.Path(sys.argv[2])


def macro(text, name):
    found = re.search(rf"^#define {name} (\d+)u$", text, flags=re.M)
    assert found, f"{name} is not defined as an unsigned literal"
    return int(found.group(1))


def declarations(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"\\\n", " ", text)
    text = re.sub(r"^[ \t]*#.*$", "", text, flags=re.M)
    text = re.sub(r'extern "C" \{|^\}$', "", text, flags=re.M)
    return [" ".join(part.split()) for part in text.split(";") if part.strip()]


def macros(text):
    joined = re.sub(r"\\\n", " ", text)
    return {m.group(1): " ".join(m.group(2).split())
            for m in re.finditer(r"^#define (MAELYS_OCI_\w+)\b(.*)$", joined, flags=re.M)}


def enumeration(declaration):
    found = re.fullmatch(r"typedef enum (\w+) \{(.*)\} (\w+)", declaration)
    if not found:
        return None
    return found.group(1), found.group(3), [m.strip() for m in found.group(2).split(",") if m.strip()]


def honoured(old, current):
    """What of `old` the current declarations no longer carry."""
    present = set(current)
    enumerations = {e[0]: e for e in map(enumeration, current) if e}
    lost = []
    for declaration in old:
        if declaration in present:
            continue
        before = enumeration(declaration)
        after = enumerations.get(before[0]) if before else None
        if after and after[1] == before[1] and after[2][:len(before[2])] == before[2]:
            continue
        lost.append(declaration)
    return lost


def main():
    current = HEADER.read_text()
    version = macro(current, "MAELYS_OCI_ABI_VERSION")
    floor = macro(current, "MAELYS_OCI_ABI_COMPATIBLE_SINCE")
    assert 1 <= floor <= version, (floor, version)
    frozen = FROZEN / f"abi-{floor}.h"
    assert frozen.is_file(), f"{frozen} must freeze the header of revision {floor}"
    old = frozen.read_text()
    assert macro(old, "MAELYS_OCI_ABI_VERSION") == floor, f"{frozen} is not revision {floor}"

    lost = honoured(declarations(old), declarations(current))
    assert not lost, "revision %d is no longer honoured:\n  %s" % (floor, "\n  ".join(lost))
    now = macros(current)
    moved = {name: value for name, value in macros(old).items()
             if name != "MAELYS_OCI_ABI_VERSION" and now.get(name) != value}
    assert not moved, f"macros of revision {floor} changed or disappeared: {sorted(moved)}"

    # The check itself must tell a break from an addition.
    sample = declarations(old)
    function = next(d for d in sample if d.endswith(")") and "(" in d)
    assert honoured(sample, [d for d in declarations(current) if d != function]) == [function]
    changed = function.replace("(", "(int extra, ", 1)
    assert honoured(sample, [changed if d == function else d
                             for d in declarations(current)]) == [function]
    listing = next(d for d in sample if enumeration(d))
    name, alias, members = enumeration(listing)
    grown = f"typedef enum {name} {{ {', '.join(members + ['MAELYS_OCI_LATER'])} }} {alias}"
    shrunk = f"typedef enum {name} {{ {', '.join(members[1:])} }} {alias}"
    rest = [d for d in sample if d != listing]
    assert honoured(sample, rest + [grown]) == []
    assert honoured(sample, rest + [shrunk]) == [listing]
    # Every public enumeration has its frozen consumer.
    consumer = (FROZEN / "enumerations.c").read_text()
    for listing in filter(None, map(enumeration, declarations(current))):
        absent = [m.split("=")[0].strip() for m in listing[2]
                  if f"case {m.split('=')[0].strip()}:" not in consumer]
        assert not absent, f"{FROZEN}/enumerations.c does not switch on {absent} of {listing[1]}"
    print(f"PASS revision {version} honours every declaration of revision {floor} "
          f"({len(sample)} declarations, {len(macros(old)) - 1} macros)")


if __name__ == "__main__":
    main()
