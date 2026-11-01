#!/bin/sh
# End-to-end framing test over stdin/stdout (--tty -). IRIX /bin/sh is Bourne: no $(...).
agent=${1:-./qemu-guest-agent}
tmp=${TMPDIR:-/tmp}/qemu-guest-agent-stdio.$$
trap 'rm -f $tmp.got $tmp.want $tmp.file' 0

# Partial garbage, then 0xFF resync, then a real request: only the request is answered.
printf '{"execute":"guest-p\377{"execute":"guest-sync-delimited","arguments":{"id":42}}\n{"execute":"guest-ping"}\n' \
    | "$agent" --tty - 2>/dev/null > $tmp.got
printf '\377{"return":42}\n{"return":{}}\n' > $tmp.want
if cmp -s $tmp.got $tmp.want; then
    echo "stdio framing ok"
else
    echo "FAIL stdio framing"
    exit 1
fi

# stat applet: both spellings the host uses, and a missing path.
f=$tmp.file
printf 'hello' > $f
chmod 644 $f
bsd=`"$agent" --applet stat -f "%z %p %m" $f` || { echo "FAIL stat -f"; exit 1; }
gnu=`"$agent" --applet stat -c "%s %f %Y" $f` || { echo "FAIL stat -c"; exit 1; }
set -- $bsd; [ "$1" = 5 ] && [ "$2" = 100644 ] || { echo "FAIL stat -f output: $bsd"; exit 1; }
set -- $gnu; [ "$1" = 5 ] && [ "$2" = 81a4 ] || { echo "FAIL stat -c output: $gnu"; exit 1; }
if "$agent" --applet stat -f "%z" $tmp.missing 2>/dev/null; then echo "FAIL stat missing"; exit 1; fi
rm -f $f
echo "stat applet ok"
