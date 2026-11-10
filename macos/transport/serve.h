/* serve.h - the agent's serve loop over all of its channels. */
#ifndef QGA_SERVE_H
#define QGA_SERVE_H

typedef struct {
    /* the agent: a serial node, "auto" (the lowest /dev/cu.virtio-*), "-" (stdin/stdout) or NULL
     * (none); a vsock port, 0 for none, vsock_explicit if it was asked for (its failures are
     * logged then; a guest with no vsock device otherwise stays quiet) */
    const char *tty;
    int         vsock_port;
    int         vsock_explicit;
    /* the log stream: the file followed (NULL: none), a serial node ("auto": the second
     * /dev/cu.virtio-*; NULL: none), a vsock port (0: none) */
    const char *log_path;
    const char *log_tty;
    int         log_vsock_port;
} qga_serve_cfg;

/* Serves until stdin ends (tty "-"); otherwise for ever, reopening what it loses. */
int qga_serve(const qga_serve_cfg *cfg);

#endif
