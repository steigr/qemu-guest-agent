/* util.h - allocation, growable string buffer, logging. C89. */
#ifndef QGA_UTIL_H
#define QGA_UTIL_H

#include <stddef.h>

/* 64-bit integer: `long long` is a GNU extension that GCC 2.95 accepts. */
typedef long long qga_i64;

void *qga_xmalloc(size_t n);
void *qga_xrealloc(void *p, size_t n);
char *qga_xstrdup(const char *s);
char *qga_xstrndup(const char *s, size_t n);

typedef struct {
    char *p;
    size_t len;
    size_t cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_reset(sbuf *b);
void sb_putc(sbuf *b, int c);
void sb_append(sbuf *b, const char *s, size_t n);
void sb_puts(sbuf *b, const char *s);
const char *sb_cstr(sbuf *b);   /* NUL-terminated view, valid until next write */

/* snprintf into an error buffer; always returns 1 so handlers can `return qga_seterr(...)`. */
int qga_seterr(char *err, size_t errlen, const char *fmt, ...);

/* Logging goes to stderr until qga_log_open() succeeds. */
void qga_log_open(const char *path);
void qga_log(const char *fmt, ...);
extern int qga_verbose;     /* --verbose: log every request and response */

#endif
