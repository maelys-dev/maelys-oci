#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The TLS dependency gate rejects vulnerable headers and runtimes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tests/security/check_mbedtls_version.c"
VERSION_SOURCE = ROOT / "src/puller/tls_version.c"
CC = shlex.split(os.environ.get("CC", "cc"))


def compile_case(base: Path, header_version: int, runtime_version: int):
    include = base / "include/mbedtls"
    include.mkdir(parents=True, exist_ok=True)
    (include / "version.h").write_text(
        "#ifndef TEST_MBEDTLS_VERSION_H\n"
        "#define TEST_MBEDTLS_VERSION_H\n"
        f"#define MBEDTLS_VERSION_NUMBER 0x{header_version:08x}\n"
        "unsigned int mbedtls_version_get_number(void);\n"
        "#endif\n")
    runtime = base / "runtime.c"
    runtime.write_text(
        f"unsigned int mbedtls_version_get_number(void) "
        f"{{ return 0x{runtime_version:08x}u; }}\n")
    executable = base / "check"
    return subprocess.run(
        [*CC, "-std=c11", "-Wall", "-Wextra", "-Werror",
         f"-I{base / 'include'}", f"-I{ROOT}", str(VERSION_SOURCE),
         str(SOURCE), str(runtime), "-o", str(executable)],
        capture_output=True, text=True), executable


def main():
    with tempfile.TemporaryDirectory(prefix="maelys-oci-mbedtls.") as temporary:
        base = Path(temporary)
        vulnerable_build, _ = compile_case(
            base / "vulnerable-build", 0x021C0900, 0x021C0900)
        assert vulnerable_build.returncode != 0

        vulnerable_three_build, _ = compile_case(
            base / "vulnerable-three-build", 0x03060200, 0x03060200)
        assert vulnerable_three_build.returncode != 0

        mismatched_build, mismatched = compile_case(
            base / "vulnerable-runtime", 0x021C0A00, 0x021C0900)
        assert mismatched_build.returncode == 0, mismatched_build.stderr
        assert subprocess.run([mismatched], capture_output=True).returncode != 0

        secure_two_build, secure_two = compile_case(
            base / "secure-two", 0x021C0A00, 0x021C0A00)
        assert secure_two_build.returncode == 0, secure_two_build.stderr
        assert subprocess.run([secure_two], capture_output=True).returncode == 0

        secure_build, secure = compile_case(
            base / "secure", 0x03060300, 0x03060300)
        assert secure_build.returncode == 0, secure_build.stderr
        assert subprocess.run([secure], capture_output=True).returncode == 0
    print("PASS vulnerable Mbed TLS headers and runtimes are rejected")


if __name__ == "__main__":
    main()
