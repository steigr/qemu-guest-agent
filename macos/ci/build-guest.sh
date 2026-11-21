# In the guest, in the repository: build-guest.sh <variant> <dl> <deps> <stage>
#
# variant   gcc2-10.0   10.0's GCC 2.95.2 and headers (/opt/toolkit-10.0), linked against 10.0's libraries
#                       (/opt/sysroot-10.0) in a flat namespace, as 10.0's own linker would
#           gcc2-10.1   the same with 10.1's (/opt/toolkit-10.1, /opt/sysroot-10.1)
#           gcc3-10.2   Panther's GCC 3.3 and headers, linked against 10.2's libraries (/opt/sysroot-10.2)
#           gcc3-10.3   Panther's GCC 3.3: the PowerPC agent for 10.3 (and 10.4 and 10.5)
#           intel       the Snow Leopard guest's Xcode 3.2.6 (`make intel`): the Intel releases' one binary, i386 and
#                       x86_64, with a BUILD-INFO of what built it
#
# The Panther guest is 10.3.9; the older releases' toolkits are its virtio-drivers side toolkits
# (macos/ci/side-toolkit.sh there), and their sysroots are made from each release's install CD by
# ci/sysroot.sh. Linking against the release's own libraries is what makes the build that release's:
# a symbol it does not have fails the link here, and not the agent's launch on the guest.
#
# The self-test runs here, on 10.3.9. Built for 10.0, its native netstat table reads 10.0's structures
# and gets 10.3's, so two of its checks fail as they are meant to and the build goes on (a real
# netstat answers on such a mismatch); the binary is checked on 10.0 itself by the add-ons' gauntlet.
set -e
make clean >/dev/null 2>&1 || true
variant=$1
toolkit_cc() {
  # 2.95.2's cc finds cc1obj, cc1plus and as under /usr/libexec/gcc, which is Panther's; its cpp-precomp
  # reads a specs file Panther has no copy of (virtio-drivers' side-toolkit.sh, proven 2026-10-19).
  t=/opt/toolkit-$1
  [ -x "$t/usr/bin/cc" ] || { echo "no toolkit at $t" >&2; exit 1; }
  mkdir -p "$TMPDIR/bin"
  cat > "$TMPDIR/bin/cc" <<WRAP
#!/bin/sh
exec $t/usr/bin/cc -B$t/usr/libexec/gcc/darwin/ppc/2.95.2/ -B$t/usr/libexec/gcc/darwin/ppc/ -no-cpp-precomp "\$@"
WRAP
  chmod +x "$TMPDIR/bin/cc"
  echo "$TMPDIR/bin/cc"
}
sysroot() {
  [ -f "/opt/sysroot-$1/usr/lib/libSystem.B.dylib" ] || { echo "no sysroot at /opt/sysroot-$1 (ci/sysroot.sh)" >&2; exit 1; }
  echo "/opt/sysroot-$1"
}
case "$variant" in
intel)
  make intel
  build/intel/qemu-guest-agent --self-test | tail -1
  cp build/intel/qemu-guest-agent "$4/"
  {
    echo "built on: `sw_vers -productVersion` `sw_vers -buildVersion`"
    gcc-4.0 --version | head -1
    gcc-4.2 --version | head -1
    echo "binary: `lipo -info build/intel/qemu-guest-agent | sed 's/.*: //'`"
  } > "$4/BUILD-INFO"
  ;;
gcc2-10.0|gcc2-10.1)
  rel=${variant#gcc2-}
  cc=`toolkit_cc $rel`
  flat=
  # 10.0's dyld has no two-level namespace.
  [ "$rel" = 10.0 ] && flat=-Wl,-flat_namespace
  make CC="$cc" TOOLKIT=/opt/toolkit-$rel SYSROOT=`sysroot $rel` LDEXTRA="$flat"
  ./qemu-guest-agent --self-test | tail -1 || true
  otool -L qemu-guest-agent
  cp qemu-guest-agent "$4/"
  ;;
gcc3-10.2)
  make SYSROOT=`sysroot 10.2`
  ./qemu-guest-agent --self-test | tail -1
  otool -L qemu-guest-agent
  cp qemu-guest-agent "$4/"
  ;;
gcc3-10.3)
  make
  ./qemu-guest-agent --self-test | tail -1
  cp qemu-guest-agent "$4/"
  ;;
*)
  echo "unknown variant $variant" >&2
  exit 2
  ;;
esac
