#!/bin/sh
# On the runner: fetch-sources.sh <dl dir>   (component.sh's SOURCES, one "name|url|sha256|filter" per line)
#
# Each file is downloaded, checked against its SHA-256 and, for filter "bz2", recompressed as gzip under the
# name given (the guests have no bzcat). The checksum is of the file as downloaded.
set -eu
dl=$1
mkdir -p "$dl"
printf '%s\n' "$SOURCES" | while IFS='|' read -r name url sha filter; do
	[ -n "$name" ] || continue
	curl -fsSL -m 600 -o "$dl/.download" "$url"
	have=$(sha256sum "$dl/.download" | cut -d' ' -f1)
	if [ "$sha" != "-" ] && [ "$have" != "$sha" ]; then
		echo "fetch-sources: $url is $have, expected $sha" >&2
		exit 1
	fi
	case "$filter" in
	bz2) bzcat "$dl/.download" | gzip -9 > "$dl/$name" ;;
	*) mv "$dl/.download" "$dl/$name" ;;
	esac
	rm -f "$dl/.download"
	echo "fetched $name ($have)"
done
