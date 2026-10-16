#!/bin/sh
# Managed by maelys-release. Regenerate with
# 'maelys-release adopt' of maelys-release; do not edit by hand.
#
# usage: scripts/checkout-dependencies.sh DESTINATION
#
# Clones every dependencies/*.pin of this repository at its commit into
# DESTINATION/NAME, one clone per pin through scripts/checkout-dependency.sh,
# and prints one line, MAELYS_DEPENDENCIES_DIR=<absolute DESTINATION>, for a
# shell to take whole:
#
#     sh scripts/checkout-dependencies.sh "$RUNNER_TEMP/dependencies" >>"$GITHUB_ENV"
#
# The build reads that variable and nothing beside the repository. Every job
# that builds runs this, because a variable in $GITHUB_ENV reaches no other
# job: each runs on another machine, where a path of the first names nothing.
# The trial of 0.45.0 is why this exists -- the socle gave the root in its
# own three places and the six jobs maelys-egress owns failed on the
# product's own message, because nothing had given it to them.
#
# The destination is given and never chosen here: a script that picked one
# would be the ambient default again, one level down. On a developer's
# machine 'maelys-release dependencies DIR --apply' is the tool -- it
# refreshes, and refuses a working copy; this clones into what is not there
# yet, which is what a runner and a container offer.
#
# Every pin, never a list of names: the jobs of maelys-egress named three
# dependencies in five places each, and a fourth pin would have been the one
# forgotten in one of them.
#
# Every pin but one kind: a pin whose file says 'on-request' is skipped here
# and cloned only when a job asks for it by name, through
# scripts/checkout-dependency.sh NAME. maelys-http pins Mbed TLS twice, and
# its second pin serves one job; every other job cloned it, submodules and
# all, without reading a file of it. The exception is declared in the pin,
# which is still the one place a dependency is named.
#
# And each clone says how long it took, on stderr: that cost was invisible,
# and a product had to compare two runs to see forty seconds a job.
set -eu
destination=${1:?usage: scripts/checkout-dependencies.sh DESTINATION}
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
mkdir -p -- "$destination"
destination=$(CDPATH='' cd -- "$destination" && pwd)
found=
for pin_file in "$root"/dependencies/*.pin; do
    test -f "$pin_file" || continue
    found=1
    name=${pin_file##*/}
    name=${name%.pin}
    # Reported on stderr with the clones themselves: stdout carries the one
    # assignment, and a second line there would land in $GITHUB_ENV.
    if sed -n '3,$p' "$pin_file" | grep -q '^on-request$'; then
        echo "checkout-dependencies: $name is cloned on request only (dependencies/$name.pin): skipped" >&2
        continue
    fi
    started=$(date +%s)
    sh "$root/scripts/checkout-dependency.sh" "$name" "$destination/$name" >&2
    echo "checkout-dependencies: $name cloned in $(($(date +%s) - started))s" >&2
done
# The socle is the last dependency that was still read beside the product.
# It is not a dependencies/*.pin -- its commit is the one on the `uses:` line
# of release.yml, or of ci.yml for a product that publishes by its own means
# -- but it is read like one: a Makefile target that checks conformance took
# it from ../maelys-release, a working copy that moves as much as any other.
# CI already clones it at that commit; this gives the same root to a machine,
# so the two agree instead of differing in the one place nobody looks.
socle_pin=$(sed -n 's|.*maelys-release/.github/workflows/release\.yml@\([0-9a-f]\{40\}\)\(.*\)|\1\2|p' \
    "$root/.github/workflows/release.yml" 2>/dev/null | head -n 1)
if [ -z "$socle_pin" ]; then
    socle_pin=$(sed -n 's|.*maelys-release/.github/workflows/check-product\.yml@\([0-9a-f]\{40\}\)\(.*\)|\1\2|p' \
        "$root"/.github/workflows/*.yml 2>/dev/null | head -n 1)
fi
socle_sha=$(printf '%s\n' "$socle_pin" | cut -c 1-40)
# The same line names the tag, in its trailing comment: `@<commit> # vX.Y.Z`.
# A fetch by commit brings no tag, so the checkout did not know which version
# it was: `cut` run from it said "socle untagged" every time, and `adopt`
# refused it without --allow-untagged. The tag is brought by that name, as a
# label and nothing more:
#
#   - the commit on the line decides what is checked out, alone and first;
#   - a comment that is not a release tag -- `# untagged`, which a candidate
#     under trial writes, or no comment at all -- asks the remote nothing;
#   - the remote is asked what the tag names before anything of it is
#     fetched, and a tag that names another commit is not fetched at all;
#   - the tag is created here only when what was fetched resolves, peeled,
#     to that commit AND, for a tag object, calls itself by the name asked
#     for. `git describe` answers with the name inside the object, not with
#     the name of the ref: a remote serving refs/tags/v1.2.3 as an object
#     named v9.0.0 would make this checkout say it is v9.0.0;
#   - whatever is declined is said on stderr, and fails nothing.
#
# The two calls that ask for the tag give up on a remote that accepts and
# then says nothing, through git's own low-speed settings: measured, such a
# call is cut after the time below with "Operation too slow". They bound an
# http(s) transfer, which is what the fleet's base is. The fetch by commit
# further down is NOT bounded that way, and neither is a clone of a pin: a
# remote that hangs there holds the job, as it did before this.
socle_tag=$(printf '%s\n' "$socle_pin" \
    | sed -n 's|^[0-9a-f]\{40\} # \(v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)[[:space:]]*$|\1|p')
socle_git() {
    git -C "$destination/maelys-release" -c http.lowSpeedLimit=1000 -c http.lowSpeedTime=10 "$@"
}
# Attempted, never required. The pins are what a build needs; the socle root
# is what a conformance target reads, and a machine that cannot reach the
# socle must still be able to build. A failure says so on stderr and leaves
# stdout with the one assignment that matters.
if [ -n "$socle_sha" ] && [ ! -e "$destination/maelys-release" ]; then
    base=${MAELYS_GIT_BASE:-https://github.com/maelys-dev}
    if git init --quiet "$destination/maelys-release" >&2 \
        && git -C "$destination/maelys-release" fetch --quiet --depth 1 \
            "$base/maelys-release.git" "$socle_sha" >&2 \
        && git -C "$destination/maelys-release" checkout --quiet --detach FETCH_HEAD >&2; then
        if [ -n "$socle_tag" ]; then
            if socle_listed=$(socle_git ls-remote "$base/maelys-release.git" \
                    "refs/tags/$socle_tag" "refs/tags/$socle_tag^{}"); then
                # By the whole name of the ref: the pattern of ls-remote also
                # matches a branch that ends like it.
                socle_tagged=$(printf '%s\n' "$socle_listed" | awk -v ref="refs/tags/$socle_tag" '
                    $2 == ref "^{}" { peeled = $1 }
                    $2 == ref { direct = $1 }
                    END { print (peeled != "" ? peeled : direct) }')
                if [ -z "$socle_tagged" ]; then
                    echo "checkout-dependencies: $base has no tag $socle_tag;" \
                         "the socle at $socle_sha is checked out without it" >&2
                elif [ "$socle_tagged" != "$socle_sha" ]; then
                    echo "checkout-dependencies: $base says $socle_tag is $socle_tagged" \
                         "and the uses: line pins $socle_sha: the tag is not fetched and not created," \
                         "and the socle is checked out at the commit of the line" >&2
                elif ! socle_git fetch --quiet --depth 1 \
                        "$base/maelys-release.git" "refs/tags/$socle_tag" >&2; then
                    echo "checkout-dependencies: the tag $socle_tag could not be fetched from $base;" \
                         "the socle at $socle_sha is checked out without it" >&2
                else
                    # What was fetched is judged, not what was announced.
                    socle_object=$(socle_git rev-parse --verify --quiet FETCH_HEAD 2>/dev/null || true)
                    socle_tagged=$(socle_git rev-parse --verify --quiet 'FETCH_HEAD^{commit}' 2>/dev/null || true)
                    socle_inner=$socle_tag
                    if [ "$(socle_git cat-file -t "${socle_object:-FETCH_HEAD}" 2>/dev/null || true)" = tag ]; then
                        socle_inner=$(socle_git cat-file tag "$socle_object" 2>/dev/null | sed -n '/^$/q;s/^tag //p')
                    fi
                    if [ -z "$socle_object" ] || [ "$socle_tagged" != "$socle_sha" ]; then
                        echo "checkout-dependencies: what $base served as $socle_tag is ${socle_tagged:-no commit}" \
                             "and the uses: line pins $socle_sha: the tag is not created," \
                             "and the socle is checked out at the commit of the line" >&2
                    elif [ "$socle_inner" != "$socle_tag" ]; then
                        echo "checkout-dependencies: the tag object $base serves as $socle_tag calls itself" \
                             "'$(printf '%s' "$socle_inner" | tr -cd 'A-Za-z0-9._/-' | cut -c 1-64)':" \
                             "this checkout would describe itself by that name, so the tag is not created," \
                             "and the socle is checked out at the commit of the line" >&2
                    elif ! socle_git update-ref "refs/tags/$socle_tag" "$socle_object" >&2; then
                        echo "checkout-dependencies: the tag $socle_tag could not be written;" \
                             "the socle at $socle_sha is checked out without it" >&2
                    fi
                fi
            else
                echo "checkout-dependencies: $base could not be asked for the tag $socle_tag;" \
                     "the socle at $socle_sha is checked out without it" >&2
            fi
        fi
    else
        echo "checkout-dependencies: the socle at $socle_sha could not be fetched from $base;" \
             "MAELYS_RELEASE_DIR is not set" >&2
        rm -rf -- "$destination/maelys-release"
        socle_sha=
    fi
fi
test -n "$found" || { echo "checkout-dependencies: no dependencies/*.pin in $root" >&2; exit 66; }
echo "MAELYS_DEPENDENCIES_DIR=$destination"
test -z "$socle_sha" || echo "MAELYS_RELEASE_DIR=$destination/maelys-release"
