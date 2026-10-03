/* vsock.c - libvsock, compiled in on PowerPC; stubs elsewhere (see vsock.h). */
#include "vsock.h"

#include <errno.h>

#if defined(__ppc__) || defined(__POWERPC__)

/* The library itself, from the driver's directory: one source for vsockcat and the agent. Its own
 * includes ("vsock.h", "VirtioVsockUser.h") resolve beside it. */
#include "../../../drivers/ppc/virtiovsock/vsock.c"

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

#else

int qv_listen(unsigned int port)            { (void)port; errno = ENXIO; return -1; }
int qv_accept(int l)                        { (void)l; errno = ENXIO; return -1; }
int qv_read(int h, void *b, size_t n)       { (void)h; (void)b; (void)n; errno = ENXIO; return -1; }
int qv_write(int h, const void *b, size_t n){ (void)h; (void)b; (void)n; errno = ENXIO; return -1; }
int qv_poll(int h, unsigned int *r, unsigned int *f) { (void)h; (void)r; (void)f; errno = ENXIO; return -1; }
void qv_close(int h)                        { (void)h; }
int qv_local_cid(unsigned int *hi, unsigned int *lo) { (void)hi; (void)lo; errno = ENXIO; return -1; }

#endif
