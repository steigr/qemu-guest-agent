# See openssl's component.sh. The agent's vsock transport includes ../../../drivers/virtiovsock/vsock.c, so the
# virtiovsock repository is laid out beside this one as guest/drivers/virtiovsock (SRC_DEPS) and this repository as
# guest/software/qemu-guest-agent (LAYOUT).
KIND=app
PKG=QemuGuestAgent
PACKAGE=qemu-guest-agent
VERSION=0.1
LAYOUT=guest/software/qemu-guest-agent
SRC_DEPS="virtiovsock=guest/drivers/virtiovsock"
VARIANTS="gcc3-10.3"
SOURCES=""
DEPENDS=""
STATIC=""
CHECK_PATHS="./usr/local/libexec/qemu-guest-agent"
