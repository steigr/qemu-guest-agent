#!/bin/bash
# build-intel.sh: build the Intel releases' agent (`make intel`) inside a Snow Leopard guest in the
# cluster, and bring it back with a BUILD-INFO to guest/dist/qemu-guest-agent-intel/.
#
# usage: build-intel.sh [ISO]
#
# ISO is the Snow Leopard retail DVD (default: 10.6.8, 10K549, under
# the ISO folder); its "Optional Installs" carries Xcode 3.2.6, whose packages
# are installed in the guest one by one -- the tools, both GCCs, the 10.4u and 10.5 SDKs -- unless
# the guest has them already. Attached read-only with hdiutil, so this runs on a Mac.
#
# The guest is RELEASE's pod (default guest) in NAMESPACE, which must be running with its
# agent connected. Files go in over HTTP and not through the agent: the pod serves them on its
# loopback, which passt gives the guest at its default gateway, and the agent's own serial channel
# is a UART that takes kilobytes, not hundreds of megabytes. The binary comes back as base64 on the
# agent's stdout and is checked against the guest's own SHA-256 of it.
set -euo pipefail

ISO=${1:-$ISO}
RELEASE=${RELEASE:-guest}
KUBE_CONTEXT=${KUBE_CONTEXT:-example.invalid}
NAMESPACE=${NAMESPACE:-macos}
PORT=8099
PACKAGES="DeveloperToolsSystemSupport VersionedDeveloperToolsSystemSupport DeveloperToolsCLI DevSDK gcc4.0 gcc4.2 MacOSX10.4.Universal MacOSX10.5"

here=$(cd "$(dirname "$0")" && pwd)
out=$here/../../dist/qemu-guest-agent-intel
pod() { kubectl --context "$KUBE_CONTEXT" -n "$NAMESPACE" exec -i "$RELEASE-0" -c macos -- "$@"; }
guest() { pod macos exec "$@"; }

work=$(mktemp -d)
mnt=""
cleanup() {
	[ -n "$mnt" ] && hdiutil detach -quiet "$mnt" || true
	rm -rf "$work"
}
trap cleanup EXIT

pod mkdir -p /share/build
# Asked until there is one: a guest just past its login window has no route yet.
for _ in $(seq 60); do
	gateway=$(guest /usr/sbin/netstat -rn -f inet 2>/dev/null | awk '$1 == "default" { print $2; exit }')
	[ -n "$gateway" ] && break
	sleep 5
done
[ -n "$gateway" ] || { echo "the guest has no default route after 5 minutes" >&2; exit 1; }

if ! guest /bin/sh -c 'test -x /usr/bin/gcc-4.0 -a -x /usr/bin/gcc-4.2 -a -x /usr/bin/lipo && ls -d /Developer/SDKs/MacOSX10.4u.sdk /SDKs/MacOSX10.4u.sdk 2>/dev/null | grep -q . && ls -d /Developer/SDKs/MacOSX10.5.sdk /SDKs/MacOSX10.5.sdk 2>/dev/null | grep -q .'; then
	mnt=$(hdiutil attach -readonly -nobrowse -mountrandom "$work" "$ISO" | awk -F'\t' '/Apple_HFS/ { print $NF }')
	pkgs="$mnt/Optional Installs.localized/Packages"
	for p in $PACKAGES; do cp "$pkgs/$p.pkg" "$work/"; done
	(cd "$work" && shasum -a 256 ./*.pkg | sed 's| \./| |' > SHA256SUMS)
	for f in "$work"/*.pkg "$work/SHA256SUMS"; do pod sh -c "cat > /share/build/$(basename "$f")" < "$f"; done
	echo "toolchain: Xcode 3.2.6 packages from $(basename "$ISO")" > "$work/toolchain"
else
	echo "toolchain: already installed in the guest" > "$work/toolchain"
fi

# The agent and libvsock, which it compiles in from the driver's directory, in their places under
# guest/.
COPYFILE_DISABLE=1 tar -C "$here/../.." --format ustar --no-mac-metadata --no-xattrs --exclude build \
	-czf "$work/qga.tgz" software/qemu-guest-agent drivers/virtiovsock
pod sh -c 'cat > /share/build/qga.tgz' < "$work/qga.tgz"
# Served from /share/build by a server this script started, which a nonce proves: another one on
# the port -- an earlier one serving another directory -- would hand the guest stale files.
nonce=$(uuidgen)
pod sh -c "echo $nonce > /share/build/nonce"
pod sh -c "curl -s -o /dev/null http://127.0.0.1:$PORT/ || { nohup python3 -m http.server $PORT --bind 127.0.0.1 -d /share/build >/tmp/build-http.log 2>&1 </dev/null & sleep 1; }"
[ "$(guest /usr/bin/curl -sfS "http://$gateway:$PORT/nonce")" = "$nonce" ] ||
	{ echo "port $PORT in $RELEASE-0 is served by something else than /share/build" >&2; exit 1; }

guest -i /bin/sh <<EOF
set -e
url=http://$gateway:$PORT
mkdir -p /tmp/qga-build && cd /tmp/qga-build
if curl -sfS -o SHA256SUMS \$url/SHA256SUMS; then
	for p in \$(awk '{ print \$2 }' SHA256SUMS); do curl -sfS -O \$url/\$p; done
	shasum -a 256 -c SHA256SUMS
	for p in $PACKAGES; do installer -pkg \$p.pkg -target / | tail -1; done
fi
rm -rf software drivers && curl -sfS \$url/qga.tgz | tar -xzf -
cd software/qemu-guest-agent && make intel
for a in i386 x86_64; do arch -\$a build/intel/qemu-guest-agent --self-test | tail -1; done
cp build/intel/qemu-guest-agent . && sh tests/stdio.sh ./qemu-guest-agent
EOF

mkdir -p "$out"
guest /usr/bin/openssl base64 -in /tmp/qga-build/software/qemu-guest-agent/build/intel/qemu-guest-agent | base64 -d > "$out/qemu-guest-agent.new"
want=$(guest /usr/bin/shasum -a 256 /tmp/qga-build/software/qemu-guest-agent/build/intel/qemu-guest-agent | awk '{ print $1 }')
got=$(shasum -a 256 "$out/qemu-guest-agent.new" | awk '{ print $1 }')
[ "$want" = "$got" ] || { echo "the binary came back as $got, the guest has $want" >&2; exit 1; }
mv "$out/qemu-guest-agent.new" "$out/qemu-guest-agent"
chmod 755 "$out/qemu-guest-agent"
{
	echo "qemu-guest-agent $(sed -n 's/.*QGA_VERSION "\(.*\)"/\1/p' "$here/core/dispatch.h"), make intel"
	echo "source: $(git -C "$here" rev-parse HEAD)$(git -C "$here" diff --quiet HEAD -- . ../../drivers/virtiovsock || echo ' (modified)')"
	echo "built on: $RELEASE, $(guest /usr/bin/sw_vers -productVersion) $(guest /usr/bin/sw_vers -buildVersion)"
	cat "$work/toolchain"
	guest /bin/sh -c 'gcc-4.0 --version | head -1; gcc-4.2 --version | head -1'
	echo "sha256: $got"
	echo "architectures: $(lipo -archs "$out/qemu-guest-agent")"
} > "$out/BUILD-INFO"
cat "$out/BUILD-INFO"
