/* serve.c - the agent's channels, served from one loop. Requests come a line each and 0xFF resyncs.
 *
 * A vioser port reads as end of file, and polls as hung up, while no host is connected. Nothing is
 * wrong then, so the port stays open: while the host is away it is left out of select() (which would
 * return at once) and tried on a short tick. A port is not reopened for this, since a closed port
 * tells the host's side of the device that the agent has gone. */
#include "serve.h"
#include "tty.h"
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
#define RETRY_SECONDS  2                /* port lost or not there yet */
#define ABSENT_TICK_MS 200              /* how often a port with no host is tried */
#define WRITE_STALL_MS 3000             /* a peer that drains nothing for this long loses data */
#define STATS_SECONDS  60

typedef struct {
    int    open;
    int    rfd, wfd;
    int    absent;                      /* the read said no host is connected */
    int    discarding;
    int    nowait;                      /* a log channel: what would block is dropped at once */
    unsigned long dropped;              /* bytes so dropped */
    sbuf   line;
    char   name[128];
} conn;

/* The agent channel's traffic, logged once a minute while there is any: a channel that goes quiet
 * on the host's side has to be told apart from one that is talking and not being understood. */
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

/* Never blocks for long: data the peer does not drain is dropped. */
static void conn_write(conn *c, const char *buf, size_t len)
{
    int waited = 0;

    while (len > 0) {
        ssize_t n = write(c->wfd, buf, len);

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
                c->dropped += len;
                return;
            }
            if (waited >= WRITE_STALL_MS) {
                st.stalled += (unsigned long)len;
                qga_log("%s: write stalled, dropping %lu bytes", c->name, (unsigned long)len);
                return;
            }
            /* select() returns at once on a port with no host: this has to sleep to wait. */
            msleep(10);
            waited += 10;
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
    channel ch;

    if (!c->open)
        return;
    ch.rfd = c->rfd;
    ch.wfd = c->wfd;
    tty_close(&ch);
    sb_reset(&c->line);
    c->open = 0;
}

/* (Re)discovers and opens a node, quietly while nothing changes. */
static void tty_try(conn *c, const char *tty, int log, int *last_state)
{
    char path[128];
    channel ch;

    if (strcmp(tty, "auto") == 0) {
        if (tty_discover(log, path, sizeof path) != 0) {
            if (*last_state != -1 && !log)
                qga_log("no agent port in %s yet", QGA_PORT_DIR);
            *last_state = -1;
            return;
        }
    } else {
        strncpy(path, tty, sizeof path - 1);
        path[sizeof path - 1] = '\0';
    }
    if (tty_open(path, &ch) != 0) {
        if (*last_state != 1)
            qga_log("cannot open %s: %s", path, strerror(errno));
        *last_state = 1;
        return;
    }
    *last_state = 0;
    c->open = 1;
    c->rfd = ch.rfd;
    c->wfd = ch.wfd;
    c->absent = log;                    /* a log host already there still gets the history */
    /* After (re)open, input up to the first newline or 0xFF may be a stale tail. */
    c->discarding = strcmp(path, "-") != 0;
    strncpy(c->name, path, sizeof c->name - 1);
    c->name[sizeof c->name - 1] = '\0';
    c->nowait = log;
    if (!log)
        st.opens++;
    qga_log(log ? "log stream on %s" : "serving %s", path);
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

/* A read of the agent's channel, when select() says so or on the tick while the host is away. */
static int service_agent(conn *t, int stdio, sbuf *out, time_t *retry)
{
    unsigned char buf[4096];
    ssize_t n = read(t->rfd, buf, sizeof buf);

    if (n == 0 && stdio) {
        conn_close(t);
        return 1;                       /* stdin's end is the end of a test */
    }
    if (n == 0) {
        if (!t->absent)
            qga_log("%s: no host connected", t->name);
        t->absent = 1;
        sb_reset(&t->line);
        t->discarding = 0;              /* the next connection has no stale tail */
        return 0;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        qga_log("%s: read failed: %s", t->name, strerror(errno));
        conn_close(t);
        *retry = time(NULL) + RETRY_SECONDS;
        return 0;
    }
    if (t->absent) {
        qga_log("%s: host connected", t->name);
        t->absent = 0;
    }
    if (n > 0) {
        st.in += (unsigned long)n;
        feed(t, buf, (size_t)n, out);
    }
    return 0;
}

/* Whether a host just connected to the log channel. What it sends is not read for anything else. */
static int log_host_arrived(conn *c, time_t *retry)
{
    unsigned char buf[256];
    ssize_t n = read(c->rfd, buf, sizeof buf);

    if (n == 0) {
        c->absent = 1;
        return 0;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        qga_log("%s: read failed: %s", c->name, strerror(errno));
        conn_close(c);
        *retry = time(NULL) + RETRY_SECONDS;
        return 0;
    }
    if (c->absent) {
        c->absent = 0;
        return 1;
    }
    return 0;
}

int qga_serve(const qga_serve_cfg *cfg)
{
    conn t, lt;                         /* the agent's channel, the log stream's */
    sbuf out, newlog, history;
    ls_tail tail;
    int stdio = strcmp(cfg->tty, "-") == 0;
    int tty_state = 99, ltty_state = 99;
    time_t tty_next = 0, ltty_next = 0, tail_next = 0;
    time_t stats_next = time(NULL) + STATS_SECONDS;

    memset(&t, 0, sizeof t);
    memset(&lt, 0, sizeof lt);
    sb_init(&t.line);
    sb_init(&lt.line);
    sb_init(&out);
    sb_init(&newlog);
    sb_init(&history);
    if (cfg->log_path)
        ls_tail_init(&tail, cfg->log_path);

    for (;;) {
        fd_set r, w;
        struct timeval tv;
        int maxfd = -1, rc, woken;
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
        if (!t.open && now >= tty_next) {
            tty_try(&t, cfg->tty, 0, &tty_state);
            if (!t.open)
                tty_next = now + RETRY_SECONDS;
        }
        if (cfg->log_path && cfg->log_tty && !lt.open && now >= ltty_next) {
            tty_try(&lt, cfg->log_tty, 1, &ltty_state);
            if (!lt.open)
                ltty_next = now + RETRY_SECONDS;
        }

        FD_ZERO(&r);
        FD_ZERO(&w);
        if (t.open && !t.absent) {
            FD_SET(t.rfd, &r);
            maxfd = t.rfd;
        }
        qga_poll_fill(&r, &w, &maxfd);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (t.open && t.absent) {
            tv.tv_sec = 0;
            tv.tv_usec = ABSENT_TICK_MS * 1000;
        }
        rc = select(maxfd + 1, &r, &w, NULL, &tv);
        if (rc < 0 && errno != EINTR) {
            qga_log("select failed: %s", strerror(errno));
            msleep(100);
            continue;
        }
        qga_poll_service();             /* child I/O and reaping, also on timeout */

        woken = rc > 0 && t.open && !t.absent && FD_ISSET(t.rfd, &r);
        if (t.open && (woken || t.absent) && service_agent(&t, stdio, &out, &tty_next))
            break;

        /* The log stream: new lines once a second to the log channel, while a host is there. One
         * that arrives gets the history first; that one write may wait, the rest are dropped if
         * they would block. */
        if (cfg->log_path && time(NULL) >= tail_next) {
            int arrived = lt.open && log_host_arrived(&lt, &ltty_next);

            tail_next = time(NULL) + 1;
            sb_reset(&newlog);
            if (ls_tail_poll(&tail, &newlog) > 0)
                history_add(&history, newlog.p, newlog.len);
            if (arrived && history.len > 0) {
                lt.nowait = 0;
                conn_write(&lt, history.p, history.len);
                lt.nowait = 1;
            } else if (lt.open && !lt.absent && newlog.len > 0) {
                conn_write(&lt, newlog.p, newlog.len);
            }
        }
    }

    sb_free(&t.line);
    sb_free(&lt.line);
    sb_free(&out);
    sb_free(&newlog);
    sb_free(&history);
    if (cfg->log_path)
        ls_tail_free(&tail);
    return 0;
}
