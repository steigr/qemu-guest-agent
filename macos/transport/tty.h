/* tty.h - serial channel open/discovery (the serve loop is serve.c). */
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

#endif
