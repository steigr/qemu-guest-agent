/* serve.h - the agent's serve loop over all of its channels. */
#ifndef QGA_SERVE_H
#define QGA_SERVE_H

/* Serves until stdin ends (tty "-"); otherwise for ever, reopening what it loses.
 * tty: a serial node, "auto" (the only /dev/cu.virtio-*), "-" (stdin/stdout) or NULL (none).
 * vsock_port: the vsock port to listen on, 0 for none; vsock_explicit says it was asked for, so
 * that its failures are logged (without, a guest with no vsock device stays quiet). */
int qga_serve(const char *tty, int vsock_port, int vsock_explicit);

#endif
