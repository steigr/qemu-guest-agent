#!/bin/sh
# Runner side: publish files as a generic package of the repository's owner on Forgejo.
#
#   publish-package.sh <package> <version> <file>...
#
# Each file is uploaded under <version> and again under "latest" (with the version in the
# file name replaced by "latest"), replacing what was there. The package then shows up under
# the owner's Packages, for example https://git.stei.gr/irix/-/packages. Needs PACKAGES_TOKEN
# (a token that may write packages of the owner) and the Forgejo variables of the job.
set -eu
pkg=$1; ver=$2; shift 2
owner=${GITHUB_REPOSITORY%%/*}
repo=${GITHUB_REPOSITORY#*/}
api="${GITHUB_SERVER_URL}/api/packages/${owner}/generic/${pkg}"
auth="Authorization: token ${PACKAGES_TOKEN:?no token to publish packages with}"
out=$(mktemp)
trap 'rm -f "$out"' 0

put() { # version, name, file
    code=$(curl -s -m 600 -o "$out" -w '%{http_code}' -X PUT -H "$auth" --upload-file "$3" "$api/$1/$2")
    [ "$code" = 201 ] || {
        { cat "$out"; echo; echo "publishing $2 as $pkg $1: HTTP $code"; } | tee "${PUBLISH_LOG:-/dev/null}"
        exit 1
    }
}

for v in "$ver" latest; do
    # A version cannot be uploaded twice: remove the old one (404 when there is none).
    curl -s -m 60 -o /dev/null -X DELETE -H "$auth" "$api/$v" || true
    for f in "$@"; do
        name=$(basename "$f")
        [ "$v" = latest ] && name=$(printf '%s' "$name" | sed "s/-$ver/-latest/")
        put "$v" "$name" "$f"
    done
done
# Attach the package to the repository (best effort: the page then lists it).
curl -s -m 60 -o /dev/null -X POST -H "$auth" \
    "${GITHUB_SERVER_URL}/api/v1/packages/${owner}/generic/${pkg}/-/link/${repo}" || true
echo "published $pkg $ver: ${GITHUB_SERVER_URL}/${owner}/-/packages/generic/${pkg}/${ver}"
