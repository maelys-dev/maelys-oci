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
    sh "$root/scripts/checkout-dependency.sh" "$name" "$destination/$name" >&2
done
test -n "$found" || { echo "checkout-dependencies: no dependencies/*.pin in $root" >&2; exit 66; }
echo "MAELYS_DEPENDENCIES_DIR=$destination"
