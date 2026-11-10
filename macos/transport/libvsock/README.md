# libvsock (vendored)

`vsock.c`, `vsock.h` and `VirtioVsockUser.h` are libvsock, the user-side library of the
VirtioVsock kext: stream sockets over its user client. They are copied unchanged from
[`macos/virtio_vsock`](https://git.stei.gr/virtualization/virtio-drivers/src/branch/main/macos/virtio_vsock)
of virtualization/virtio-drivers, at commit
[`7d9c55a68d`](https://git.stei.gr/virtualization/virtio-drivers/commit/7d9c55a68d51510b6bbad6694c5dc7ed92948575),
which last changed them (2026-10-12, VirtioVsock 0.2.1). `../vsock.c` compiles them in.

The interface is the kext's user client (`VirtioVsockUser.h`). It changes only with the kext, and
an agent built against an older copy keeps working with a newer kext as long as the selectors in
that header keep their numbers. To update, copy the three files again from virtio-drivers, record
the commit here, and run `make test`.
