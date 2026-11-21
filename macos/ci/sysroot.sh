#!/bin/sh
# sysroot.sh <install CD's root> <out.tar>: the libraries an older release's agent links against
# (ci/build-guest.sh), taken from that release's install CD -- the system the CD boots is the release
# as it first shipped, so a binary linked against it runs on every point release after.
#
# On a Mac, with the CD image attached and its HFS partition mounted read-only:
#
#   hdiutil attach -nomount -readonly MacOSX-10.0-4K78-ppc.iso      # note the Apple_HFS slice, e.g. disk4s9
#   mkdir cd && mount -t hfs -o rdonly /dev/disk4s9 cd
#   sh sysroot.sh cd sysroot-10.0.tar
#
# and in the Panther guest of the macosx-ppc runner, once per release, as root:
#
#   mkdir -p /opt/sysroot-10.0 && tar -xf sysroot-10.0.tar -C /opt/sysroot-10.0
#
# Made so for 10.0 (4K78), 10.1 (5G64) and 10.2 (6C115) on 2026-11-21. The CD's crt1.o is not
# needed: Panther's start code asks libSystem for nothing 10.0's lacks (atexit, exit, errno,
# __keymgr_dwarf2_register_sections), and 10.0's and 10.1's CDs have none.
set -eu
cd_root=$1
out=$2
case "$out" in /*) ;; *) out=$PWD/$out ;; esac
cd "$cd_root"
files="usr/lib/libSystem.B.dylib usr/lib/libSystem.dylib usr/lib/libc.dylib usr/lib/libm.dylib usr/lib/libpthread.dylib
  usr/lib/libinfo.dylib"
for f in IOKit CoreFoundation; do
  files="$files System/Library/Frameworks/$f.framework/$f System/Library/Frameworks/$f.framework/Versions/Current
    System/Library/Frameworks/$f.framework/Versions/A/$f"
done
for f in $files; do
  [ -e "$f" ] || [ -h "$f" ] || { echo "sysroot.sh: $cd_root has no $f" >&2; exit 1; }
done
# ustar and no extended attributes: Panther's tar reads nothing newer.
COPYFILE_DISABLE=1 tar --format=ustar --no-xattrs -cf "$out" $files 2>/dev/null || COPYFILE_DISABLE=1 tar -cf "$out" $files
echo "sysroot.sh: $out"
