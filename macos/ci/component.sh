# What macos/ci builds for the agent: its installer package (PKG), the registry's package (PACKAGE), the variants and
# what the check looks for. The version is QGA_VERSION of core/dispatch.h. libvsock is vendored in
# transport/libvsock, so macos/ builds on its own, at LAYOUT in the guest's payload. A variant is the release the
# binary is for (ci/build-guest.sh): one each for 10.0, 10.1, 10.2 and 10.3, as the installer kits of
# steigr/qemu-vm-container-macos-guest carry one agent per release.
PKG=QemuGuestAgent
PACKAGE=macos_qemu_guest_agent
LAYOUT=qemu-guest-agent
VARIANTS="gcc2-10.0 gcc2-10.1 gcc3-10.2 gcc3-10.3"
STATIC=""
CHECK_PATHS="./usr/local/libexec/qemu-guest-agent"
