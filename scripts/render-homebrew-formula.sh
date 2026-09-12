#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Render one of packaging/homebrew/{libmaelys-oci,maelys-oci}.rb.in for a
# released tag.
# usage: scripts/render-homebrew-formula.sh vX.Y.Z [OUTPUT [FORMULA]]
# FORMULA is libmaelys-oci (the library) or maelys-oci (the terminal); it
# defaults to maelys-oci. The source archive of the tag is downloaded to
# compute its digest, and the maelys-cli pin is read from the tag's own
# dependencies/ file, so the formula builds the framework the release was
# verified with. Maelys System, JSON and HTTP come from the tap's own
# formulas; the Makefile verifies them against the tag's pins at build time.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
tag=${1:?usage: render-homebrew-formula.sh vX.Y.Z [OUTPUT [FORMULA]]}
formula=${3:-maelys-oci}
output=${2:-$root/dist/homebrew/$formula.rb}
case $formula in
    libmaelys-oci|maelys-oci) ;;
    *) echo "unknown formula: $formula" >&2; exit 64 ;;
esac
repository=${MAELYS_SOURCE_REPOSITORY:-maelys-dev/maelys-oci}
version=${tag#v}
url="https://github.com/$repository/archive/refs/tags/$tag.tar.gz"
temp_base=${TMPDIR:-/tmp}
temp_base=${temp_base%/}
work=$(mktemp -d "$temp_base/maelys-oci-formula.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
curl -fsSL --retry 5 --retry-delay 3 -o "$work/source.tar.gz" "$url"
digest=$(shasum -a 256 "$work/source.tar.gz" | awk '{print $1}')
mkdir -p "$work/tag"
tar -xzf "$work/source.tar.gz" -C "$work/tag" --strip-components=1
cli_tag=$(sed -n '1p' "$work/tag/dependencies/maelys-cli.pin")
cli_pin=$(sed -n '2p' "$work/tag/dependencies/maelys-cli.pin")
test "$(cat "$work/tag/VERSION")" = "$version" || {
    echo "tag $tag carries VERSION $(cat "$work/tag/VERSION")" >&2
    exit 1
}
mkdir -p "$(dirname "$output")"
sed -e "s|@URL@|$url|g" -e "s|@VERSION@|$version|g" -e "s|@SHA256@|$digest|g" \
    -e "s|@CLI_TAG@|$cli_tag|g" \
    -e "s|@CLI_PIN@|$cli_pin|g" \
    "$work/tag/packaging/homebrew/$formula.rb.in" >"$output"
grep -q '@[A-Z_]*@' "$output" && { echo "unrendered placeholder in $output" >&2; exit 1; }
printf '%s\n' "rendered $output"
