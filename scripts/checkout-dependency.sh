#!/bin/sh
# Managed by maelys-release. Regenerate with
# 'maelys-release adopt' of maelys-release; do not edit by hand.
#
# usage: scripts/checkout-dependency.sh NAME [DESTINATION]
#
# Clones the repository NAME next to this one at the commit on line 2 of
# dependencies/NAME.pin. Line 1 is for humans, and this script only prints
# it: the tag that commit carries -- or, for a commit that carries none,
# what `git describe --tags` says of it, or under an `unreleased` line the
# last tag before it. `maelys-release dependencies` holds line 1 to line 2.
# The release socle runs this before packaging, the product's CI before
# `make check`, a developer once by hand. It refuses to replace an existing
# DESTINATION.
#
# A Maelys repository needs nothing more: MAELYS_GIT_BASE (default
# https://github.com/maelys-dev) prefixes the clone URL. A dependency that
# lives elsewhere declares where, on a line `repository <https URL>` of its
# pin: a product that must build a third-party library from a pinned commit
# says so once, in the same file as every other pin.
#
# A third party's commit that no tag carries says so, on a line `unreleased
# <branch>`: it is on that branch, on purpose. It is cloned here like any
# other commit, and nothing here reads the branch: `maelys-release
# dependencies DIR --apply` asks that repository whether the branch still
# holds the commit, and refuses the day it does not.
#
# A dependency whose build needs its submodules adds a line `submodules`, or
# `submodules recursive`. It is declared and not automatic: a submodule's
# commit is pinned by its superproject, but its URL comes from that
# repository's .gitmodules, which this pin does not name, so initialising
# one fetches from a repository the product never declared.
#
# A dependency a runner may not fetch -- a private repository, on a machine
# that holds no credential for it -- can travel instead: when
# MAELYS_DEPENDENCY_BUNDLES names a directory holding NAME.bundle, that git
# bundle is cloned in place of the repository. The commit is checked the same
# way, by its own hash, so a bundle brings the pinned commit or nothing. A
# bundle carries no submodule, and a pin that declares them is refused rather
# than built without them. carry-dependencies.yml of maelys-release makes
# those bundles, on a machine that may read.
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
# A line that means a repository and is not one the reader below matches --
# a space after the URL, a space or a tab before the word, another case, no
# URL -- was no line at all, and the clone then came from the organisation:
# another repository than the one the file names. It is refused, with its
# line, and so is a second repository line, since only the first is read.
# Nothing else is refused here: what this script does not read, it ignores.
unread=$(awk 'NR > 2 && tolower($0) ~ /^[ \t]*repository/ {
    seen += 1
    if ($0 !~ /^repository  *https:\/\/[^ ]*$/) { print NR " unread"; exit }
    if (seen > 1) { print NR " second"; exit }
}' "$pin_file")
case $unread in
    *' unread')
        echo "checkout-dependency: $pin_file line ${unread%% *} starts like a repository line and is not one:" \
             "'repository <https URL>' from the first column, with nothing after the URL. Nothing is cloned:" \
             "the organisation's repository of that name is not the one this file means" >&2
        exit 65 ;;
    *' second')
        echo "checkout-dependency: $pin_file line ${unread%% *} is a second repository line, and one is read." \
             "Nothing is cloned" >&2
        exit 65 ;;
esac
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
bundle=
if [ -n "${MAELYS_DEPENDENCY_BUNDLES:-}" ] && [ -f "$MAELYS_DEPENDENCY_BUNDLES/$name.bundle" ]; then
    bundle=$MAELYS_DEPENDENCY_BUNDLES/$name.bundle
fi
if [ -n "$bundle" ]; then
    if sed -n '3,$p' "$pin_file" | grep -q '^submodules'; then
        echo "checkout-dependency: $pin_file declares submodules, which a bundle does not carry" >&2
        exit 65
    fi
    git clone --quiet --no-checkout "$bundle" "$destination"
    # The remote a clone of the repository would have: whatever runs after
    # this sees one shape of checkout, not two.
    git -C "$destination" remote set-url origin "$repository"
    source="bundle $bundle"
else
    git clone --quiet --filter=blob:none --no-checkout "$repository" "$destination"
    source=$repository
fi
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
echo "$name $tag ($pin) from $source in $destination"
