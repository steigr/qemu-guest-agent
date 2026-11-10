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

/* The lowest-numbered /dev/cu.virtio-* node (the agent's), or with tty_discover_nth the nth from
 * the lowest (1: the log stream's). Returns 0 and fills out, -1 if there is none. */
int tty_discover(char *out, size_t outlen);
int tty_discover_nth(int nth, char *out, size_t outlen);

#endif
