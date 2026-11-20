# In the guest, in the repository: build-guest.sh <variant> <dl> <deps> <stage>
#
# variant   gcc3-10.3   Panther's GCC 3.3: the PowerPC agent
#           intel       the Snow Leopard guest's Xcode 3.2.6 (`make intel`): the Intel releases' one binary, i386 and
#                       x86_64, with a BUILD-INFO of what built it
set -e
make clean >/dev/null 2>&1 || true
case "$1" in
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
*)
  make
  ./qemu-guest-agent --self-test | tail -1
  cp qemu-guest-agent "$4/"
  ;;
esac
