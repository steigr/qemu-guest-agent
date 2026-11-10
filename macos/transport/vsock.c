/* vsock.c - libvsock, compiled in (see vsock.h). */
#include "vsock.h"

#include <errno.h>

/* The library itself, vendored from virtio-drivers' macos/virtio_vsock (libvsock/README.md). Its own
 * includes ("vsock.h", "VirtioVsockUser.h") resolve beside it. */
#include "libvsock/vsock.c"

int qv_listen(unsigned int port)            { return vsock_listen(port, 1); }
int qv_accept(int l)                        { return vsock_accept(l, 0, 0, 0, 1); }
int qv_read(int h, void *buf, size_t n)     { return vsock_read(h, buf, n, 1); }
int qv_write(int h, const void *b, size_t n){ return vsock_write(h, b, n, 1); }
void qv_close(int h)                        { vsock_close(h); }
int qv_local_cid(unsigned int *hi, unsigned int *lo) { return vsock_local_cid(hi, lo); }

int qv_poll(int h, unsigned int *readable, unsigned int *flags)
{
    unsigned int w;

    return vsock_poll(h, readable, &w, flags);
}
