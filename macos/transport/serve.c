/* serve.c - the agent's channels, served together from one loop: the virtio console (or another
 * serial node, or stdin/stdout) and a vsock port. Each channel frames requests on its own (a line
 * per request, 0xFF resyncs); the commands' state -- open files, exec'd children, the freeze -- is
 * one, whichever channel a request came by.
 *
 * Serial nodes are file descriptors and wait in select(). vsock handles are not (libvsock is an
 * IOKit user client), so while vsock is in use select() waits only a short tick and the handles
 * are polled after it. */
#include "serve.h"
#include "tty.h"
#include "vsock.h"

#include "../core/dispatch.h"
#include "../core/util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE       (256 * 1024)
#define DELIM          0xFF
#define RETRY_SECONDS  2                /* serial node lost or not there yet */
#define VSOCK_RETRY    10               /* no vsock device (yet: the kext may load after us) */
#define VSOCK_TICK_MS  20               /* select()'s wait while vsock handles are polled */
#define WRITE_STALL_MS 3000             /* a peer that drains nothing for this long loses data */

typedef struct {
    int    open;
    int    vsock;                       /* the kind: a vsock handle in vh, or fds */
    int    rfd, wfd, vh;
    int    discarding;
    sbuf   line;
    char   name[64];
} conn;

static void msleep(int ms)
{
    struct timeval tv;

    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
}

/* Never blocks for long: data the peer does not drain is dropped (plan 4.1: a host that does not
 * listen looks like a slow one). */
static void conn_write(conn *c, const char *buf, size_t len)
{
    int waited = 0;

    while (len > 0) {
        ssize_t n;

        if (c->vsock)
            n = qv_write(c->vh, buf, len);
        else
            n = write(c->wfd, buf, len);
        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            waited = 0;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            if (waited >= WRITE_STALL_MS) {
                qga_log("%s: write stalled, dropping %lu bytes", c->name, (unsigned long)len);
                return;
            }
            if (c->vsock) {
                msleep(10);
                waited += 10;
            } else {
                fd_set w;
                struct timeval tv;

                FD_ZERO(&w);
                FD_SET(c->wfd, &w);
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                select(c->wfd + 1, NULL, &w, NULL, &tv);
                waited += 1000;
            }
            continue;
        }
        qga_log("%s: write failed: %s", c->name, strerror(errno));
        return;
    }
}

static void process_line(conn *c, const char *line, size_t len, sbuf *out)
{
    size_t s = 0;

    while (s < len && (line[s] == ' ' || line[s] == '\t' || line[s] == '\r'))
        s++;
    if (s == len)
        return;                         /* blank line */
    if (qga_verbose)
        qga_log("%s <- %.*s", c->name, (int)(len - s > 300 ? 300 : len - s), line + s);
    sb_reset(out);
    qga_handle(line + s, len - s, out);
    if (qga_verbose)
        qga_log("%s -> %.*s", c->name, (int)(out->len > 300 ? 300 : out->len), out->p);
    if (out->len > 0)
        conn_write(c, out->p, out->len);
}

static void feed(conn *c, const unsigned char *buf, size_t n, sbuf *out)
{
    size_t k;

    for (k = 0; k < n; k++) {
        unsigned char ch = buf[k];

        if (ch == DELIM) {              /* peer resync: drop partial input */
            sb_reset(&c->line);
            c->discarding = 0;
        } else if (ch == '\n') {
            if (!c->discarding)
                process_line(c, c->line.p ? c->line.p : "", c->line.len, out);
            sb_reset(&c->line);
            c->discarding = 0;
        } else if (!c->discarding) {
            if (c->line.len >= MAX_LINE) {
                qga_log("%s: request line too long, discarding", c->name);
                sb_reset(&c->line);
                c->discarding = 1;
            } else {
                sb_putc(&c->line, ch);
            }
        }
    }
}

static void conn_close(conn *c)
{
    if (!c->open)
        return;
    if (c->vsock) {
        qv_close(c->vh);
    } else {
        channel ch;

        ch.rfd = c->rfd;
        ch.wfd = c->wfd;
        tty_close(&ch);
    }
    sb_reset(&c->line);
    c->open = 0;
}

/* The serial side: (re)discovers and opens the node, quietly while nothing changes. */
static void tty_try(conn *c, const char *tty, int *last_state)
{
    char path[256];
    channel ch;
    int state;

    if (strcmp(tty, "auto") == 0) {
        state = tty_discover(path, sizeof path);
        if (state != 0) {
            if (state != *last_state)
                qga_log(state == -2 ? "several /dev/cu.virtio-* nodes; pin one with --tty"
                                    : "no /dev/cu.virtio-* node yet");
            *last_state = state;
            return;
        }
    } else {
        strncpy(path, tty, sizeof path - 1);
        path[sizeof path - 1] = '\0';
    }
    if (tty_open(path, &ch) != 0) {
        if (*last_state != 1)
            qga_log("cannot open %s", path);
        *last_state = 1;
        return;
    }
    *last_state = 0;
    c->open = 1;
    c->vsock = 0;
    c->rfd = ch.rfd;
    c->wfd = ch.wfd;
    /* After (re)open, input up to the first newline or 0xFF may be a stale tail (plan 4.1). */
    c->discarding = strcmp(path, "-") != 0;
    strncpy(c->name, path, sizeof c->name - 1);
    c->name[sizeof c->name - 1] = '\0';
    qga_log("serving %s", path);
}

int qga_serve(const char *tty, int vsock_port, int vsock_explicit)
{
    conn t, v;
    sbuf out;
    int stdio = tty && strcmp(tty, "-") == 0;
    int tty_state = 99, listener = -1, vsock_logged = 0;
    time_t tty_next = 0, vsock_next = 0;
    unsigned char buf[4096];

    memset(&t, 0, sizeof t);
    memset(&v, 0, sizeof v);
    sb_init(&t.line);
    sb_init(&v.line);
    sb_init(&out);

    for (;;) {
        fd_set r, w;
        struct timeval tv;
        int maxfd = -1, rc;
        time_t now = time(NULL);

        if (tty && !t.open && now >= tty_next) {
            tty_try(&t, tty, &tty_state);
            if (!t.open)
                tty_next = now + RETRY_SECONDS;
        }
        if (vsock_port > 0 && listener < 0 && now >= vsock_next) {
            listener = qv_listen((unsigned int)vsock_port);
            if (listener >= 0) {
                unsigned int hi = 0, lo = 0;

                qv_local_cid(&hi, &lo);
                qga_log("serving vsock port %d (cid %u)", vsock_port, lo);
                vsock_logged = 0;
            } else {
                /* auto: no device is the usual case, said once; asked for, every failure */
                if (vsock_explicit || (!vsock_logged && errno != ENXIO))
                    qga_log("vsock port %d: %s", vsock_port, strerror(errno));
                vsock_logged = 1;
                vsock_next = now + VSOCK_RETRY;
            }
        }

        FD_ZERO(&r);
        FD_ZERO(&w);
        if (t.open) {
            FD_SET(t.rfd, &r);
            maxfd = t.rfd;
        }
        qga_poll_fill(&r, &w, &maxfd);
        tv.tv_sec = 0;
        tv.tv_usec = VSOCK_TICK_MS * 1000;
        if (listener < 0) {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        }
        rc = select(maxfd + 1, &r, &w, NULL, &tv);
        if (rc < 0 && errno != EINTR) {
            qga_log("select failed: %s", strerror(errno));
            msleep(100);
            continue;
        }
        qga_poll_service();             /* child I/O and reaping, also on timeout */

        if (t.open && rc > 0 && FD_ISSET(t.rfd, &r)) {
            ssize_t n = read(t.rfd, buf, sizeof buf);

            if (n > 0) {
                feed(&t, buf, (size_t)n, &out);
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)) {
                if (n == 0 && stdio) {
                    conn_close(&t);
                    break;              /* stdin's end is the end of a test */
                }
                qga_log(n == 0 ? "%s returned EOF" : "%s: read failed", t.name);
                conn_close(&t);
                qga_log("channel lost, reconnecting");
                tty_next = time(NULL) + RETRY_SECONDS;
            }
        }

        if (listener >= 0) {
            unsigned int readable = 0, flags = 0;

            if (qv_poll(listener, &readable, &flags) < 0 || (flags & QV_RESET)) {
                qga_log("vsock listener lost (%s)", strerror(errno));
                conn_close(&v);
                qv_close(listener);
                listener = -1;
                vsock_next = time(NULL) + 1;
            } else if (flags & QV_ACCEPT) {
                int h = qv_accept(listener);

                if (h >= 0) {
                    if (v.open)
                        qga_log("%s replaced by a new connection", v.name);
                    conn_close(&v);
                    v.open = 1;
                    v.vsock = 1;
                    v.vh = h;
                    v.discarding = 0;   /* a new stream has no stale tail */
                    sprintf(v.name, "vsock:%d", vsock_port);
                    qga_log("vsock connection accepted");
                }
            }
        }
        if (v.open) {
            unsigned int readable = 0, flags = 0;

            if (qv_poll(v.vh, &readable, &flags) < 0) {
                conn_close(&v);
            } else {
                while (readable > 0) {
                    int n = qv_read(v.vh, buf, sizeof buf);

                    if (n <= 0)
                        break;
                    feed(&v, buf, (size_t)n, &out);
                    readable = readable > (unsigned int)n ? readable - (unsigned int)n : 0;
                }
                if ((flags & QV_RESET) || ((flags & QV_EOF) && readable == 0)) {
                    qga_log("vsock connection closed");
                    conn_close(&v);
                }
            }
        }
    }

    sb_free(&t.line);
    sb_free(&v.line);
    sb_free(&out);
    if (listener >= 0)
        qv_close(listener);
    return 0;
}
