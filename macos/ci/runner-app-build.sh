#!/bin/sh
# On the Forgejo runner beside the Panther guest: runner-app-build.sh <variant> <payload dir> <stage dir>
#
# <payload dir> is laid out as guest-app-build.sh wants (the repository at $LAYOUT, dl/, deps/). It goes onto the
# 9p share, the guest builds it over ssh (127.0.0.1:22, the pod's key from the ssh-agent), and the stage it leaves
# is unpacked into <stage dir>.
set -eu
variant=$1
payload=$2
stage=$3
job=app-$variant-$$-$(date +%s)
share=/share/$job
mkdir -p "$share"
chmod 777 "$share"
ssh_guest() {
	ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
		-o ConnectTimeout=15 $GUEST_SSH "$@"
}
# The guest's files on the share are the guest account's, which the runner's rm cannot take.
cleanup() {
	ssh_guest "rm -rf /Volumes/share/$job" 2>/dev/null || true
	rm -rf "$share" 2>/dev/null || true
}
trap cleanup EXIT
tar -C "$payload" -czf "$share/payload.tgz" .
cp "$payload/${LAYOUT:-repo}/ci/component.sh" "$payload/${LAYOUT:-repo}/ci/guest-app-build.sh" "$share/"
mkdir -p "$share/out"
chmod 777 "$share/out"
chmod 644 "$share"/*.tgz "$share"/*.sh
n=0
until ssh_guest true; do
	n=$((n + 1))
	[ "$n" -lt 40 ] || { echo "the guest did not answer ssh" >&2; exit 1; }
	sleep 5
done
ssh_guest "sh -c 'sh /Volumes/share/$job/guest-app-build.sh $variant /Volumes/share/$job 2>&1'"
mkdir -p "$stage"
tar -C "$stage" -xzf "$share/out/$variant.tgz"
