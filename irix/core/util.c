#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Out of memory is fatal: the service wrapper restarts the agent. */
static void oom(void)
{
    fputs("qemu-guest-agent: out of memory\n", stderr);
    exit(1);
}

void *qga_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *qga_xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *qga_xstrndup(const char *s, size_t n)
{
    char *d = (char *)qga_xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *qga_xstrdup(const char *s)
{
    return qga_xstrndup(s, strlen(s));
}

void sb_init(sbuf *b)
{
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(sbuf *b)
{
    free(b->p);
    sb_init(b);
}

void sb_reset(sbuf *b)
{
    b->len = 0;
}

static void sb_reserve(sbuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;   /* +1 keeps room for the NUL */
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < need)
            cap *= 2;
        b->p = (char *)qga_xrealloc(b->p, cap);
        b->cap = cap;
    }
}

void sb_putc(sbuf *b, int c)
{
    sb_reserve(b, 1);
    b->p[b->len++] = (char)c;
}

void sb_append(sbuf *b, const char *s, size_t n)
{
    sb_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

void sb_puts(sbuf *b, const char *s)
{
    sb_append(b, s, strlen(s));
}

const char *sb_cstr(sbuf *b)
{
    sb_reserve(b, 0);
    b->p[b->len] = '\0';
    return b->p;
}

/* Up to two logs: --log may be given twice. */
#define MAX_LOGS 2
static FILE *log_fps[MAX_LOGS];
static int log_count = 0;
int qga_verbose = 0;
static int log_paused = 0;

void qga_log_pause(int paused)
{
    log_paused = paused;
}

void qga_log_open(const char *path)
{
    FILE *fp = fopen(path, "a");

    if (!fp)
        fprintf(stderr, "qemu-guest-agent: cannot open log %s\n", path);
    else if (log_count < MAX_LOGS)
        log_fps[log_count++] = fp;
    else
        fclose(fp);
}

void qga_log(const char *fmt, ...)
{
    char ts[32], msg[1024];
    time_t now = time(NULL);
    va_list ap;
    int k;

    if (log_paused)
        return;
    if (strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now)) == 0)
        ts[0] = '\0';
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (log_count == 0) {
        fprintf(stderr, "%s qemu-guest-agent: %s\n", ts, msg);
        fflush(stderr);
    }
    for (k = 0; k < log_count; k++) {
        fprintf(log_fps[k], "%s qemu-guest-agent: %s\n", ts, msg);
        fflush(log_fps[k]);
    }
}

int qga_seterr(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return 1;
}
