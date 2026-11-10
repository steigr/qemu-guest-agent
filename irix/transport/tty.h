/* tty.h - channel open/discovery (the serve loop is serve.c). */
#ifndef QGA_TTY_H
#define QGA_TTY_H

#include <stddef.h>

#define QGA_PORT_DIR "/hw/virtio-ports"     /* vioser's named ports */

typedef struct {
    int rfd;
    int wfd;
} channel;

/* Opens PATH non-blocking; a tty is also made raw (8N1, CLOCAL, no echo/flow control). PATH "-"
 * uses stdin/stdout, for testing without a port. Returns 0/-1. */
int tty_open(const char *path, channel *ch);
void tty_close(channel *ch);

/* The agent's port, or with log the log stream's: the first of its names that exists. Returns 0 and
 * fills out, -1 if there is none (the driver is not loaded, or the host did not wire the port). */
int tty_discover(int log, char *out, size_t outlen);

#endif
