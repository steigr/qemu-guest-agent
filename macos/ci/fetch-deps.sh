#!/bin/sh
# On the runner: fetch-deps.sh <variant> <deps dir>   (component.sh's DEPENDS, "package:PkgName" each)
#
# The newest upload of each dependency's package in this owner's generic registry, as its stage
# (<PkgName>-<variant>-<version>.stage.tar.gz), unpacked into <deps dir>/<package>/. Needs $JWT with read:package.
set -eu
variant=$1
deps=$2
server=$FORGEJO_SERVER
owner=$FORGEJO_OWNER
mkdir -p "$deps"
for d in $DEPENDS; do
	p=${d%%:*}
	k=${d##*:}
	ver=$(curl -fsS -H "Authorization: bearer $JWT" "$server/api/v1/packages/$owner?type=generic&q=$p&limit=50" |
		jq -r --arg p "$p" '[.[] | select(.name == $p)] | sort_by(.created_at) | last | .version')
	[ -n "$ver" ] && [ "$ver" != null ] || { echo "fetch-deps: no $p in $owner's registry" >&2; exit 1; }
	file="$k-$variant-${ver%.*}.stage.tar.gz"
	mkdir -p "$deps/$p"
	curl -fsS -H "Authorization: bearer $JWT" "$server/api/packages/$owner/generic/$p/$ver/$file" | tar -C "$deps/$p" -xzf -
	echo "dependency $p $ver ($file)"
done
