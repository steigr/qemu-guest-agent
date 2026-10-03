/* vsock.h - the agent's view of libvsock (drivers/ppc/virtiovsock): stream sockets over
 * VirtioVsock.kext. Handles are not file descriptors, so the serve loop polls them. Built in on
 * PowerPC only: libvsock calls an IOKit user client through IOConnectMethodScalarIScalarO, which a
 * current host SDK no longer has; elsewhere every call fails with ENXIO, as on a guest without the
 * device. */
#ifndef QGA_VSOCK_H
#define QGA_VSOCK_H

#include <stddef.h>

#define QGA_VSOCK_PORT 7700             /* the port the Perl agent's TCP fallback had */

int  qv_listen(unsigned int port);      /* a handle, or -1 (ENXIO: no device) */
int  qv_accept(int listener);           /* a handle, or -1 (EAGAIN: none waiting) */
int  qv_read(int h, void *buf, size_t n);           /* without blocking; 0 at the end */
int  qv_write(int h, const void *buf, size_t n);    /* without blocking; how many went */
/* readable bytes and flags: QV_CONNECTED, QV_EOF, QV_RESET, QV_ACCEPT, QV_NOSEND */
int  qv_poll(int h, unsigned int *readable, unsigned int *flags);
void qv_close(int h);
int  qv_local_cid(unsigned int *hi, unsigned int *lo);

#define QV_CONNECTED 0x01
#define QV_EOF       0x02
#define QV_RESET     0x04
#define QV_ACCEPT    0x08
#define QV_NOSEND    0x10

#endif
