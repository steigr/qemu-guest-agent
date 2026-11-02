#!/bin/sh
# Run in the IRIX guest, from the repository: build and self-test the agent, then make its inst
# package (gendist exists only there).
#
#   tools/irix-package.sh <artifact-dir>
#
# Writes <artifact-dir>/qemu_guest_agent-<version>.tardist. inst versions are integers: 0.1.0 is 100.
art=$1
prod=qemu_guest_agent
sub=$prod.sw.base
bin=/tmp/qemu-guest-agent.obj/qemu-guest-agent
ver=`sed -n 's/.*QGA_VERSION "\(.*\)".*/\1/p' core/dispatch.h`
inst_ver=`echo $ver | awk -F. '{ print $1 * 10000 + $2 * 100 + $3 }'`
tmp=/var/tmp/irix-package-$$
trap 'cd /; rm -rf $tmp' 0

smake clean || exit 1
smake || exit 1
smake test || exit 1

mkdir -p $tmp/root/usr/local/libexec $tmp/root/etc/init.d $tmp/root/etc/config $tmp/out || exit 1
cp $bin $tmp/root/usr/local/libexec/qemu-guest-agent || exit 1
$bin --print-rc-script > $tmp/root/etc/init.d/qemu-guest-agent || exit 1
echo on > $tmp/root/etc/config/qemu-guest-agent || exit 1

# The same files as --install, but inst's to upgrade and remove. Only the running system gets its
# agent started or stopped, not one under inst -r. A removeop runs when the files have gone, so it
# cannot use the rc script.
rc=/etc/init.d/qemu-guest-agent
pidfile=/var/run/qemu-guest-agent.pid
if_live='if test x$rbase = x/ -o x$rbase = x; then'
kill_agent='kill `cat '$pidfile' 2>/dev/null` 2>/dev/null; rm -f '$pidfile
sort -k 5,6 > $tmp/$prod.idb <<IDB
f 0755 root sys usr/local/libexec/qemu-guest-agent usr/local/libexec/qemu-guest-agent $sub
f 0755 root sys etc/init.d/qemu-guest-agent etc/init.d/qemu-guest-agent $sub exitop("$if_live $rc stop; $rc start; fi") removeop("$if_live $kill_agent; fi")
f 0644 root sys etc/config/qemu-guest-agent etc/config/qemu-guest-agent $sub config(noupdate)
l 0777 root sys etc/rc2.d/S99qemu-guest-agent etc/rc2.d/S99qemu-guest-agent $sub symval(../init.d/qemu-guest-agent)
l 0777 root sys etc/rc0.d/K01qemu-guest-agent etc/rc0.d/K01qemu-guest-agent $sub symval(../init.d/qemu-guest-agent)
IDB

cat > $tmp/$prod.spec <<SPEC
product $prod
    id "QEMU guest agent $ver for IRIX 6.5"
    image sw
        id "QEMU guest agent $ver"
        version $inst_ver
        order 9999
        subsys base default
            id "qemu-guest-agent, started at boot"
            replaces $sub 0 $inst_ver
            exp $sub
        endsubsys
    endimage
endproduct
SPEC

(cd $tmp && /usr/sbin/gendist -sbase root -idb $prod.idb -spec $prod.spec -dist out) || exit 1
(cd $tmp/out && tar cf ../$prod-$ver.tardist *) || exit 1
mkdir -p $art || exit 1
cp $tmp/$prod-$ver.tardist $art/ || exit 1
ls -l $art/$prod-$ver.tardist
