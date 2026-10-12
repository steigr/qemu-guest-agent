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
#include "logstream.h"

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
/* A UART -- the Intel releases' pci-serial, /dev/cu.*serial* -- is read every UART_TICK_MS whether
 * or not select() says so, and reopened after UART_IDLE seconds without a request. Both are
 * mav2287/mac-guest-agent's measurements on these releases, which this agent replaces there:
 * Tiger's serial driver can stop waking select() while the fd stays valid (its issue #10: 300 of
 * 300 samples in select, bytes queued, never read), and input queued past ~1.3 KB before anyone
 * reads it is lost, so a request larger than the driver's buffer arrives only if it is drained
 * every ~20 ms. A host that pings never leaves the channel idle that long; one that has gone costs
 * a reopen. The virtio console needs neither: select() is woken by its own driver. */
#define UART_TICK_MS   20
#define UART_IDLE      600
#define STATS_SECONDS  60

typedef struct {
    int    open;
    int    vsock;                       /* the kind: a vsock handle in vh, or fds */
    int    rfd, wfd, vh;
    int    discarding;
    int    nowait;                      /* a log channel: what would block is dropped at once */
    int    uart;                        /* a serial port, not the virtio console: polled */
    time_t last_request;                /* when a request last arrived, for UART_IDLE */
    unsigned long dropped;              /* bytes so dropped */
    sbuf   line;
    char   name[64];
} conn;

/* The agent channel's traffic, logged once a minute while there is any: a channel that goes quiet
 * on the host's side has to be told apart from one that is talking and not being understood, and
 * on 2026-10-09 neither El Capitan's nor Lion's agent could be asked which it was. */
static struct {
    unsigned long in, requests, out, stalled, resyncs, discarded, opens;
} st, st_logged;

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
            if (!c->nowait)
                st.out += (unsigned long)n;
            buf += n;
            len -= (size_t)n;
            waited = 0;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
            if (c->nowait) {
                /* nobody on the host drains this channel (yet): the log is not the agent's
                 * business to wait for, and a console with no reader would stall it */
                c->dropped += len;
                return;
            }
            if (waited >= WRITE_STALL_MS) {
                st.stalled += (unsigned long)len;
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
    c->last_request = time(NULL);
    st.requests++;
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
            st.resyncs++;
            if (c->line.len > 0)
                st.discarded++;
            sb_reset(&c->line);
            c->discarding = 0;
        } else if (ch == '\n') {
            if (c->discarding)
                st.discarded++;
            else
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

/* The serial side: (re)discovers and opens the node, quietly while nothing changes. "auto" is the
 * nth /dev/cu.virtio-* (0 the agent's, 1 the log stream's). */
static void tty_try(conn *c, const char *tty, int nth, int *last_state)
{
    char path[256];
    channel ch;

    if (strcmp(tty, "auto") == 0) {
        if (tty_discover_nth(nth, path, sizeof path) != 0) {
            if (*last_state != -1 && nth == 0)
                qga_log("no /dev/cu.virtio-* node yet");
            *last_state = -1;
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
    c->nowait = nth != 0;
    c->uart = strcmp(path, "-") != 0 && strncmp(path, "/dev/cu.virtio-", 15) != 0;
    c->last_request = time(NULL);
    if (nth == 0)
        st.opens++;
    qga_log(nth == 0 ? "serving %s" : "log stream on %s", path);
}

/* A vsock listener: (re)made when there is none, every VSOCK_RETRY seconds; quiet where there is
 * no device unless the port was asked for. */
static void vsock_try(int *listener, int port, int explicit_, time_t *next, int *logged, const char *what)
{
    time_t now = time(NULL);

    if (port <= 0 || *listener >= 0 || now < *next)
        return;
    *listener = qv_listen((unsigned int)port);
    if (*listener >= 0) {
        unsigned int hi = 0, lo = 0;

        qv_local_cid(&hi, &lo);
        qga_log("%s on vsock port %d (cid %u)", what, port, lo);
        *logged = 0;
        return;
    }
    if (explicit_ || (!*logged && errno != ENXIO))
        qga_log("vsock port %d: %s", port, strerror(errno));
    *logged = 1;
    *next = now + VSOCK_RETRY;
}

/* Accepts on a listener, replacing the connection there was: a host that reconnects wants the new
 * one. Returns 1 when a connection was accepted. Loses the listener (and the connection) on a reset. */
static int vsock_service(int *listener, conn *c, int port, time_t *next, const char *what)
{
    unsigned int readable = 0, flags = 0;
    int h;

    if (*listener < 0)
        return 0;
    if (qv_poll(*listener, &readable, &flags) < 0 || (flags & QV_RESET)) {
        qga_log("vsock port %d lost (%s)", port, strerror(errno));
        conn_close(c);
        qv_close(*listener);
        *listener = -1;
        *next = time(NULL) + 1;
        return 0;
    }
    if (!(flags & QV_ACCEPT) || (h = qv_accept(*listener)) < 0)
        return 0;
    if (c->open)
        qga_log("%s replaced by a new connection", c->name);
    conn_close(c);
    c->open = 1;
    c->vsock = 1;
    c->vh = h;
    c->discarding = 0;                  /* a new stream has no stale tail */
    c->nowait = strcmp(what, "agent") != 0;
    sprintf(c->name, "vsock:%d", port);
    qga_log("%s: connection accepted", what);
    return 1;
}

/* Keeps the last QGA_LOG_HISTORY bytes of the log, cut at line starts. */
static void history_add(sbuf *h, const char *p, size_t n)
{
    sb_append(h, p, n);
    if (h->len > QGA_LOG_HISTORY) {
        size_t cut = h->len - QGA_LOG_HISTORY;

        while (cut < h->len && h->p[cut - 1] != '\n')
            cut++;
        memmove(h->p, h->p + cut, h->len - cut);
        h->len -= cut;
    }
}

int qga_serve(const qga_serve_cfg *cfg)
{
    conn t, v, lt, lv;                  /* agent: serial, vsock; log stream: serial, vsock */
    sbuf out, newlog, history;
    ls_tail tail;
    int stdio = cfg->tty && strcmp(cfg->tty, "-") == 0;
    int tty_state = 99, ltty_state = 99, listener = -1, llistener = -1, vlogged = 0, llogged = 0;
    time_t tty_next = 0, ltty_next = 0, vsock_next = 0, lvsock_next = 0, tail_next = 0;
    time_t stats_next = time(NULL) + STATS_SECONDS;
    unsigned char buf[4096];

    memset(&t, 0, sizeof t);
    memset(&v, 0, sizeof v);
    memset(&lt, 0, sizeof lt);
    memset(&lv, 0, sizeof lv);
    sb_init(&t.line);
    sb_init(&v.line);
    sb_init(&lt.line);
    sb_init(&lv.line);
    sb_init(&out);
    sb_init(&newlog);
    sb_init(&history);
    if (cfg->log_path)
        ls_tail_init(&tail, cfg->log_path);

    for (;;) {
        fd_set r, w;
        struct timeval tv;
        int maxfd = -1, rc;
        time_t now = time(NULL);

        if (now >= stats_next) {
            stats_next = now + STATS_SECONDS;
            if (memcmp(&st, &st_logged, sizeof st) != 0) {
                qga_log("channel: %lu bytes in, %lu requests, %lu bytes out, %lu dropped unsent, "
                        "%lu resyncs, %lu partial lines discarded, %lu opens (totals)",
                        st.in, st.requests, st.out, st.stalled, st.resyncs, st.discarded, st.opens);
                st_logged = st;
            }
        }
        if (cfg->tty && !t.open && now >= tty_next) {
            tty_try(&t, cfg->tty, 0, &tty_state);
            if (!t.open)
                tty_next = now + RETRY_SECONDS;
        }
        vsock_try(&listener, cfg->vsock_port, cfg->vsock_explicit, &vsock_next, &vlogged, "agent");
        if (cfg->log_path) {
            if (cfg->log_tty && !lt.open && now >= ltty_next) {
                tty_try(&lt, cfg->log_tty, 1, &ltty_state);
                if (lt.open && t.open && strcmp(lt.name, t.name) == 0)
                    conn_close(&lt);    /* the one console there is is the agent's */
                if (!lt.open)
                    ltty_next = now + RETRY_SECONDS;
            }
            vsock_try(&llistener, cfg->log_vsock_port, 0, &lvsock_next, &llogged, "log stream");
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
        if (listener < 0 && llistener < 0 && !(t.open && t.uart)) {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        } else if (t.open && t.uart) {
            tv.tv_usec = UART_TICK_MS * 1000;
        }
        rc = select(maxfd + 1, &r, &w, NULL, &tv);
        if (rc < 0 && errno != EINTR) {
            qga_log("select failed: %s", strerror(errno));
            msleep(100);
            continue;
        }
        qga_poll_service();             /* child I/O and reaping, also on timeout */

        if (t.open && t.uart && now - t.last_request >= UART_IDLE) {
            qga_log("%s: no request for %d s, reopening", t.name, UART_IDLE);
            conn_close(&t);
            tty_next = now;
        }
        if (t.open && ((rc > 0 && FD_ISSET(t.rfd, &r)) || t.uart)) {
            int woken = rc > 0 && FD_ISSET(t.rfd, &r);
            ssize_t n = read(t.rfd, buf, sizeof buf);

            /* A UART read when select() did not say so: with VMIN 0 a non-blocking tty read
             * returns 0 when there is nothing, which is not an end. */
            if (n > 0) {
                st.in += (unsigned long)n;
                feed(&t, buf, (size_t)n, &out);
                while (t.uart && n == (ssize_t)sizeof buf && (n = read(t.rfd, buf, sizeof buf)) > 0) {
                    st.in += (unsigned long)n;
                    feed(&t, buf, (size_t)n, &out);
                }
            } else if ((n == 0 && (woken || !t.uart)) ||
                       (n < 0 && errno != EAGAIN && errno != EINTR && errno != EWOULDBLOCK)) {
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

        vsock_service(&listener, &v, cfg->vsock_port, &vsock_next, "agent");
        if (v.open) {
            unsigned int readable = 0, flags = 0;

            if (qv_poll(v.vh, &readable, &flags) < 0) {
                conn_close(&v);
            } else {
                while (readable > 0) {
                    int n = qv_read(v.vh, buf, sizeof buf);

                    if (n <= 0)
                        break;
                    st.in += (unsigned long)n;
                    feed(&v, buf, (size_t)n, &out);
                    readable = readable > (unsigned int)n ? readable - (unsigned int)n : 0;
                }
                if ((flags & QV_RESET) || ((flags & QV_EOF) && readable == 0)) {
                    qga_log("agent: vsock connection closed");
                    conn_close(&v);
                }
            }
        }

        /* The log stream: new lines once a second, to every log channel that is open; a host that
         * connects over vsock gets the history first. What the host sends there is not read. */
        if (cfg->log_path) {
            if (vsock_service(&llistener, &lv, cfg->log_vsock_port, &lvsock_next, "log stream") &&
                history.len > 0)
                conn_write(&lv, history.p, history.len);
            if (lv.open) {
                unsigned int readable = 0, flags = 0;

                if (qv_poll(lv.vh, &readable, &flags) < 0 || (flags & (QV_RESET | QV_EOF | QV_NOSEND))) {
                    qga_log("log stream: vsock connection closed");
                    conn_close(&lv);
                }
            }
            if (now >= tail_next) {
                tail_next = now + 1;
                sb_reset(&newlog);
                if (ls_tail_poll(&tail, &newlog) > 0) {
                    history_add(&history, newlog.p, newlog.len);
                    if (lv.open)
                        conn_write(&lv, newlog.p, newlog.len);
                    if (lt.open)
                        conn_write(&lt, newlog.p, newlog.len);
                }
            }
        }
    }

    sb_free(&t.line);
    sb_free(&v.line);
    sb_free(&lt.line);
    sb_free(&lv.line);
    sb_free(&out);
    sb_free(&newlog);
    sb_free(&history);
    if (cfg->log_path)
        ls_tail_free(&tail);
    if (listener >= 0)
        qv_close(listener);
    if (llistener >= 0)
        qv_close(llistener);
    return 0;
}
