/* tty.h - serial channel open/discovery and the framed read loop. */
#ifndef QGA_TTY_H
#define QGA_TTY_H

#include <stddef.h>

typedef struct {
    int rfd;
    int wfd;
} channel;

/* Opens PATH raw (8N1, CLOCAL, no echo/flow control, non-blocking). PATH "-"
 * uses stdin/stdout, for testing without a serial device. Returns 0/-1. */
int tty_open(const char *path, channel *ch);
void tty_close(channel *ch);

/* Looks for /dev/cu.virtio-* nodes. Returns 0 and fills out when there is
 * exactly one, -1 if none, -2 if several (candidates are logged). */
int tty_discover(char *out, size_t outlen);

/* Writes without ever blocking indefinitely; drops data if the peer does not
 * drain it (see plan 4.1: host-not-listening looks the same as host-slow). */
void channel_write(const channel *ch, const char *buf, size_t len);

/* Serves requests on ch until it is lost (returns 1) or, for stdio mode,
 * input reaches EOF (returns 0). Input up to the first newline or 0xFF after
 * open is discarded, since it may be the tail of a stale request. */
int channel_serve(const channel *ch, int is_stdio);

#endif
