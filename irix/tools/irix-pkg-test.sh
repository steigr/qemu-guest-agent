#!/bin/sh
# Run in the IRIX guest: install the package with inst, check the installed agent, and remove it,
# so that a shared build guest is left without an agent at boot.
#
#   tools/irix-pkg-test.sh <tardist>
tardist=$1
prod=qemu_guest_agent
bin=/usr/local/libexec/qemu-guest-agent
pidfile=/var/run/qemu-guest-agent.pid
log=/tmp/qga-inst.log
fail() { echo "PACKAGE TEST FAILED: $*"; exit 1; }
remove() { /usr/sbin/inst -a -R $prod > $log 2>&1; }
cd / || exit 1
trap 'remove; rm -f $log' 0

/usr/sbin/inst -a -f $tardist -I $prod > $log 2>&1
tail -3 $log
grep 'Installations and removals were successful' $log > /dev/null || fail "inst: `grep -i error $log | head -3`"
/usr/sbin/versions -n $prod | cat

$bin --version || fail "the installed agent does not run"
$bin --self-test > $log 2>&1 || { tail -20 $log; fail "the installed agent's self-test"; }
/etc/chkconfig qemu-guest-agent || fail "not on in chkconfig"
for f in /etc/rc2.d/S99qemu-guest-agent /etc/rc0.d/K01qemu-guest-agent; do
    [ -x $f ] || fail "no $f"
done
sleep 2
pid=`cat $pidfile 2>/dev/null`
[ -n "$pid" ] && kill -0 $pid 2>/dev/null || fail "inst did not start the agent"

remove
grep 'Installations and removals were successful' $log > /dev/null || { tail -5 $log; fail "removing it"; }
sleep 1
kill -0 $pid 2>/dev/null && fail "removing it left the agent running"
for f in $bin /etc/init.d/qemu-guest-agent /etc/config/qemu-guest-agent /etc/rc2.d/S99qemu-guest-agent /etc/rc0.d/K01qemu-guest-agent; do
    [ -f $f ] || [ -h $f ] && fail "removing it left $f"
done
echo "PACKAGE TEST OK"
