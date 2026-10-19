# In the guest, in the repository: build-guest.sh <variant> <dl> <deps> <stage>
set -e
make clean >/dev/null 2>&1 || true
make
./qemu-guest-agent --self-test | tail -1
cp qemu-guest-agent "$4/"
