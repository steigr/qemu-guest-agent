# What macos/ci builds for the agent: its installer package (PKG), the registry's package (PACKAGE), the variants and
# what the check looks for. The version is QGA_VERSION of core/dispatch.h. libvsock is vendored in
# transport/libvsock, so macos/ builds on its own, at LAYOUT in the guest's payload.
PKG=QemuGuestAgent
PACKAGE=macos_qemu_guest_agent
LAYOUT=qemu-guest-agent
VARIANTS="gcc3-10.3"
STATIC=""
CHECK_PATHS="./usr/local/libexec/qemu-guest-agent"
