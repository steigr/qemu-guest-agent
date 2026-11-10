# What macos/ci builds for the agent: its installer package (PKG), the registry's package (PACKAGE), the variants and
# what the check looks for. The version is QGA_VERSION of core/dispatch.h.
#
# The agent's vsock transport includes ../../../drivers/virtiovsock/vsock.c, the libvsock of
# virtualization/virtio-drivers' macos/virtio_vsock, so the build lays macos/ out as guest/software/qemu-guest-agent
# (LAYOUT) and that folder beside it as guest/drivers/virtiovsock (VSOCK_LAYOUT), the layout this agent was
# written in.
PKG=QemuGuestAgent
PACKAGE=macos_qemu_guest_agent
LAYOUT=guest/software/qemu-guest-agent
VSOCK_REPO=virtio-drivers
VSOCK_PATH=macos/virtio_vsock
VSOCK_LAYOUT=guest/drivers/virtiovsock
VARIANTS="gcc3-10.3"
STATIC=""
CHECK_PATHS="./usr/local/libexec/qemu-guest-agent"
