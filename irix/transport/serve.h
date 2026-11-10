/* serve.h - the agent's serve loop over its channels. */
#ifndef QGA_SERVE_H
#define QGA_SERVE_H

typedef struct {
    /* the agent: a port or serial node, "auto" (its named port), "-" (stdin/stdout) */
    const char *tty;
    /* the log stream: the file followed (NULL: none), a node ("auto": its named port) */
    const char *log_path;
    const char *log_tty;
} qga_serve_cfg;

/* Serves until stdin ends (tty "-"); otherwise for ever, reopening what it loses. */
int qga_serve(const qga_serve_cfg *cfg);

#endif
