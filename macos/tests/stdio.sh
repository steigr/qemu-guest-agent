#!/bin/sh
# End-to-end framing test over stdin/stdout (--tty -). Plain sh/cmp only: od/hexdump
# differ across the OS X releases this runs on.
agent=${1:-./qemu-guest-agent}
tmp=${TMPDIR:-/tmp}/qemu-guest-agent-stdio.$$
trap 'rm -f $tmp.got $tmp.want' 0

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
