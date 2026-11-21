# libvsock (vendored)

`vsock.c`, `vsock.h` and `VirtioVsockUser.h` are libvsock, the user-side library of the
VirtioVsock kext: stream sockets over its user client. They are copied unchanged from
[`macos/virtio_vsock`](https://git.stei.gr/virtualization/virtio-drivers/src/branch/main/macos/virtio_vsock)
of virtualization/virtio-drivers, at commit
[`5c6f5e6`](https://git.stei.gr/virtualization/virtio-drivers/commit/5c6f5e6) (VirtioVsock 0.2.4), which
looks the kext up under its current class, `io_virtio_VirtioVsock`, and then its old one,
`gr_stei_VirtioVsock`. `../vsock.c` compiles them in.

The interface is the kext's user client (`VirtioVsockUser.h`). It changes only with the kext, and
an agent built against an older copy keeps working with a newer kext as long as the selectors in
that header keep their numbers and the class it looks for is one the kext registers. 0.3.1's copy
looked for `gr_stei_VirtioVsock` only, and VirtioVsock 0.2.3 registers `io_virtio_VirtioVsock`:
no guest with it had vsock, live 2026-11-21. To update, copy the three files again from virtio-drivers, record
the commit here, and run `make test`.
