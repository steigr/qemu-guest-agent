#!/bin/sh
# Runs in the Panther guest: guest-app-build.sh <variant> <share job dir>
#
# <share>/payload.tgz holds the repository at $LAYOUT (component.sh), the downloads in dl/ and the dependencies
# (other components' stages, unpacked) in deps/. The component's ci/build-guest.sh builds into a stage directory,
# the files of STATIC come across from the repository, and the stage goes back as <share>/out/<variant>.tgz.
# Built on the guest's own disk: the 9p share refuses the linker's chmod.
set -e
variant=$1
share=$2
. "$share/component.sh"
w=/tmp/app-build-$variant-$$
rm -rf "$w"; mkdir -p "$w"
cd "$w"
gzip -dc "$share/payload.tgz" | tar xf -
mkdir -p stage
repo="$w/${LAYOUT:-repo}"
cd "$repo"
TMPDIR="$w"; export TMPDIR
VERSION=$VERSION; export VERSION
sh ci/build-guest.sh "$variant" "$w/dl" "$w/deps" "$w/stage"
for s in $STATIC; do
  mkdir -p "$w/stage/`dirname "$s"`"
  cp -R "$repo/$s" "$w/stage/`dirname "$s"`/"
done
tar -C "$w/stage" -czf "$share/out/$variant.tgz" .
cd /
rm -rf "$w"
echo "built $variant"
