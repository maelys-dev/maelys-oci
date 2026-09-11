#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Prints the release target name of this host (linux-x86_64, linux-arm64 or
# macos-arm64), as scripts/package-release.sh and the release socle name it.
set -eu
case "$(uname -s):$(uname -m)" in
    Darwin:arm64) echo macos-arm64 ;;
    Linux:x86_64) echo linux-x86_64 ;;
    Linux:aarch64|Linux:arm64) echo linux-arm64 ;;
    *) echo "unsupported release host: $(uname -s) $(uname -m)" >&2; exit 65 ;;
esac
