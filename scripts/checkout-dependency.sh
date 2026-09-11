#!/bin/sh
# Managed by maelys-release. Regenerate with
# 'maelys-release adopt' of maelys-release; do not edit by hand.
#
# usage: scripts/checkout-dependency.sh NAME [DESTINATION]
#
# Clones the repository NAME next to this one at the commit on line 2 of
# dependencies/NAME.pin; line 1 holds the nearest tag for humans. The release
# socle runs it before packaging, the product's CI before `make check`, a
# developer once by hand. It refuses to replace an existing DESTINATION.
#
# A Maelys repository needs nothing more: MAELYS_GIT_BASE (default
# https://github.com/maelys-dev) prefixes the clone URL. A dependency that
# lives elsewhere declares where, on a line `repository <https URL>` of its
# pin: a product that must build a third-party library from a pinned commit
# says so once, in the same file as every other pin.
#
# A dependency whose build needs its submodules adds a line `submodules`, or
# `submodules recursive`. It is declared and not automatic: a submodule's
# commit is pinned by its superproject, but its URL comes from that
# repository's .gitmodules, which this pin does not name, so initialising
# one fetches from a repository the product never declared.
set -eu
name=${1:?NAME}
case $name in *[!a-z0-9-]*|'') echo "checkout-dependency: NAME must be [a-z0-9-]: $name" >&2; exit 64 ;; esac
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
destination=${2:-$root/../$name}
pin_file="$root/dependencies/$name.pin"
test -f "$pin_file" || { echo "checkout-dependency: no pin for $name: $pin_file" >&2; exit 66; }
tag=$(sed -n '1p' "$pin_file")
pin=$(sed -n '2p' "$pin_file")
printf '%s' "$pin" | grep -Eq '^[0-9a-f]{40}$' || { echo "checkout-dependency: line 2 of $pin_file must be the pinned commit" >&2; exit 65; }
repository=$(sed -n 's|^repository  *\(https://[^ ]*\)$|\1|p' "$pin_file" | head -n 1)
if [ -z "$repository" ]; then
    repository="${MAELYS_GIT_BASE:-https://github.com/maelys-dev}/$name.git"
elif sed -n '3,$p' "$pin_file" | grep -q '^repository ' && ! printf '%s' "$repository" | grep -Eq '^https://[A-Za-z0-9._~:/?#@!$&()*+,;=%-]+$'; then
    echo "checkout-dependency: the repository line of $pin_file is not an https URL" >&2
    exit 65
fi
if [ -e "$destination" ]; then
    echo "checkout-dependency: refusing to replace existing path: $destination" >&2
    exit 1
fi
git clone --quiet --filter=blob:none --no-checkout "$repository" "$destination"
git -C "$destination" checkout --quiet --detach "$pin"
test "$(git -C "$destination" rev-parse HEAD)" = "$pin"
submodules=$(sed -n 's|^submodules *\(recursive\)*$|\1|p' "$pin_file" | head -n 1)
if sed -n '3,$p' "$pin_file" | grep -q '^submodules'; then
    case $(sed -n 's|^submodules *||p' "$pin_file" | head -n 1) in
        ''|recursive) ;;
        *) echo "checkout-dependency: the submodules line of $pin_file takes nothing, or recursive" >&2; exit 65 ;;
    esac
    set -- --init
    test "$submodules" != recursive || set -- "$@" --recursive
    # Shallow first: it is far quicker, and falls back when the server
    # refuses to serve a commit it does not advertise.
    git -C "$destination" submodule update --quiet "$@" --depth 1 ||
        git -C "$destination" submodule update --quiet "$@"
fi
echo "$name $tag ($pin) from $repository in $destination"
