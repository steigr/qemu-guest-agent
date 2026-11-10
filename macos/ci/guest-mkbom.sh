#!/bin/sh
# MKPKG_BOM_VIA for mkpkg.py on a host that has no mkbom: make Archive.bom in a PowerPC guest.
#
#   MKPKG_BOM_VIA=macos/ci/guest-mkbom.sh python3 macos/ci/mkpkg.py ...
#   guest-mkbom.sh Archive.pax.gz Archive.bom
#
# The guest is reached over ssh as GUEST_SSH (user@host, the runner's key), and files go through the share the
# runner sees as SHARE_DIR and the guest as GUEST_SHARE. GUEST_PASSWORD is the guest account's, for sudo. All four
# come from the runner's Actions variables and secrets. The archive is unpacked as root in the guest (pax -p e
# keeps root:wheel and the modes) and 10.3's mkbom, which takes a directory and no list, reads the BOM off the
# tree: sizes and CRCs are the files' own.
set -eu
archive=$1
out=$2
: "${GUEST_SSH:?GUEST_SSH (user@host of the guest) is not set}"
: "${GUEST_PASSWORD:?GUEST_PASSWORD (of the guest account, for sudo) is not set}"
share=${SHARE_DIR:?SHARE_DIR (the share on the runner) is not set}
guestshare=${GUEST_SHARE:?GUEST_SHARE (the share in the guest) is not set}
job=mkbom-$$-$(date +%s)
mkdir -p "$share/$job"
chmod 777 "$share/$job"
# The guest's files on the share are the guest account's, which the runner's rm cannot take: the guest removes them.
cleanup() {
	ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
		-o ConnectTimeout=15 "$GUEST_SSH" "rm -rf $guestshare/$job" 2>/dev/null || true
	rm -rf "$share/$job" 2>/dev/null || true
}
trap cleanup EXIT
cp "$archive" "$share/$job/Archive.pax.gz"
cat > "$share/$job/run.sh" <<SH
set -e
d=/tmp/$job
rm -rf \$d; mkdir -p \$d/root
cd \$d/root
gzip -dc $guestshare/$job/Archive.pax.gz | /bin/pax -r -p e
mkbom \$d/root \$d/Archive.bom
cp \$d/Archive.bom $guestshare/$job/Archive.bom
rm -rf \$d
SH
# The password goes in on stdin, not on a command line the guest's ps shows.
printf '%s\n' "$GUEST_PASSWORD" | ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
	-o ConnectTimeout=15 "$GUEST_SSH" "sudo -S -p '' sh $guestshare/$job/run.sh 2>&1"
cp "$share/$job/Archive.bom" "$out"
