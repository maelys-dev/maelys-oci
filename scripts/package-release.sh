#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# usage: scripts/package-release.sh TARGET   (linux-x86_64 | linux-arm64 | macos-arm64)
# Checks the product, installs the terminal, the library, the manifest and
# the completions into a staging tree and leaves
# maelys-oci-VERSION-TARGET.tar.gz with its .sha256 in dist/.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$root"
target=${1:?TARGET}
version=$(sed -n '1p' VERSION)
case $version in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "VERSION must be a release SemVer, got: $version" >&2; exit 65 ;;
esac
case $target in linux-x86_64|linux-arm64|macos-arm64) ;; *) echo "unsupported target: $target" >&2; exit 64 ;; esac
case "$(uname -s):$(uname -m):$target" in
    Linux:x86_64:linux-x86_64|Linux:aarch64:linux-arm64|Linux:arm64:linux-arm64|Darwin:arm64:macos-arm64) ;;
    *) echo "target $target does not match this host" >&2; exit 65 ;;
esac
build=$("${PYTHON:-python3}" scripts/build-directory.py "${BUILD:-build/$target/release}")
# Keep cleanup ordered even when the caller exports parallel MAKEFLAGS.
make BUILD="$build" clean
make BUILD="$build" check
work=$(mktemp -d "$build/package.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
make BUILD="$build" install DESTDIR="$work/stage" PREFIX=/usr/local
mkdir -p dist
name="maelys-oci-$version-$target.tar.gz"
COPYFILE_DISABLE=1 tar -czf "dist/$name" -C "$work/stage" .
if command -v sha256sum >/dev/null 2>&1; then (cd dist && sha256sum "$name" >"$name.sha256")
else (cd dist && shasum -a 256 "$name" >"$name.sha256"); fi
printf '%s\n' "dist/$name"
