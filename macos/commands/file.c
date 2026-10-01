/* file.c - guest-file-open/close/read/write/seek/flush. Plain POSIX descriptors, no stdio. */
#include "../core/base64.h"
#include "../core/dispatch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_HANDLES 64
#define DEFAULT_READ 4096
#define MAX_READ 65536      /* keeps one reply line small enough for the channel */

typedef struct {
    int fd;                 /* -1: free slot */
    int is_reg;             /* regular file: a short read means EOF */
} handle;

static handle handles[MAX_HANDLES];
static int inited = 0;

static void init_table(void)
{
    int k;

    if (inited)
        return;
    for (k = 0; k < MAX_HANDLES; k++)
        handles[k].fd = -1;
    inited = 1;
}

static handle *lookup(const json *args, char *err, size_t errlen)
{
    qga_i64 h;
    int k;

    init_table();
    if (qga_need_int(args, "handle", &h, err, errlen))
        return NULL;
    for (k = 0; k < MAX_HANDLES; k++)
        if (handles[k].fd >= 0 && handles[k].fd == h)
            return &handles[k];
    qga_seterr(err, errlen, "handle \"%lld\" has not been found", h);
    return NULL;
}

/* fopen()-style mode string to open(2) flags; -1 if invalid. */
static int mode_flags(const char *m)
{
    int plus = 0, flags;
    const char *p;

    for (p = m + 1; *p; p++) {
        if (*p == '+')
            plus = 1;
        else if (*p != 'b')
            return -1;
    }
    switch (m[0]) {
    case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    return flags;
}

static int cmd_open(const json *args, json **ret, char *err, size_t errlen)
{
    const char *path = NULL, *mode = "r";
    struct stat st;
    int flags, fd, k;

    init_table();
    if (qga_need_str(args, "path", &path, err, errlen) ||
        qga_opt_str(args, "mode", &mode, err, errlen))
        return 1;
    flags = mode_flags(mode);
    if (flags < 0)
        return qga_seterr(err, errlen, "invalid file open mode '%s'", mode);
    for (k = 0; k < MAX_HANDLES && handles[k].fd >= 0; k++)
        ;
    if (k == MAX_HANDLES)
        return qga_seterr(err, errlen, "too many open files");
    fd = open(path, flags, 0666);
    if (fd < 0)
        return qga_seterr(err, errlen, "Failed to open file '%s' (mode: '%s'): %s",
                          path, mode, strerror(errno));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    handles[k].fd = fd;
    handles[k].is_reg = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    *ret = json_int(fd);
    return 0;
}

static int cmd_close(const json *args, json **ret, char *err, size_t errlen)
{
    handle *h = lookup(args, err, errlen);
    int rc;

    (void)ret;
    if (!h)
        return 1;
    rc = close(h->fd);
    h->fd = -1;
    if (rc < 0)
        return qga_seterr(err, errlen, "failed to close handle: %s", strerror(errno));
    return 0;
}

static int cmd_read(const json *args, json **ret, char *err, size_t errlen)
{
    handle *h = lookup(args, err, errlen);
    qga_i64 count = DEFAULT_READ;
    unsigned char *buf;
    ssize_t n;
    sbuf enc;
    json *o;

    if (!h || qga_opt_int(args, "count", &count, err, errlen))
        return 1;
    if (count < 0 || count > MAX_READ)
        return qga_seterr(err, errlen, "value '%lld' is invalid for argument count (0..%d)",
                          count, MAX_READ);
    buf = (unsigned char *)qga_xmalloc((size_t)count + 1);
    do
        n = read(h->fd, buf, (size_t)count);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        int e = errno;
        free(buf);
        if (e == EAGAIN) {          /* non-blocking descriptor with nothing ready */
            n = 0;
            buf = (unsigned char *)qga_xmalloc(1);
        } else {
            return qga_seterr(err, errlen, "failed to read file: %s", strerror(e));
        }
    }
    sb_init(&enc);
    base64_encode(buf, (size_t)n, &enc);
    o = json_obj();
    json_obj_set(o, "count", json_int(n));
    json_obj_set(o, "buf-b64", json_strn(sb_cstr(&enc), enc.len));
    json_obj_set(o, "eof", json_bool(h->is_reg ? n < count : n == 0));
    sb_free(&enc);
    free(buf);
    *ret = o;
    return 0;
}

static int cmd_write(const json *args, json **ret, char *err, size_t errlen)
{
    handle *h = lookup(args, err, errlen);
    const char *b64 = NULL;
    qga_i64 limit = -1;
    size_t done = 0;
    sbuf data;
    json *o;

    if (!h || qga_need_str(args, "buf-b64", &b64, err, errlen) ||
        qga_opt_int(args, "count", &limit, err, errlen))
        return 1;
    sb_init(&data);
    if (base64_decode(b64, strlen(b64), &data) != 0) {
        sb_free(&data);
        return qga_seterr(err, errlen, "invalid base64 in 'buf-b64'");
    }
    if (limit >= 0 && (size_t)limit < data.len)
        data.len = (size_t)limit;
    while (done < data.len) {
        ssize_t n = write(h->fd, data.p + done, data.len - done);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
                break;
            {
                int e = errno;
                sb_free(&data);
                return qga_seterr(err, errlen, "failed to write to file: %s", strerror(e));
            }
        }
        done += (size_t)n;
    }
    sb_free(&data);
    o = json_obj();
    json_obj_set(o, "count", json_int((qga_i64)done));
    json_obj_set(o, "eof", json_bool(0));
    *ret = o;
    return 0;
}

static int cmd_seek(const json *args, json **ret, char *err, size_t errlen)
{
    handle *h = lookup(args, err, errlen);
    qga_i64 off = 0;
    const json *w = json_obj_get(args, "whence");
    int whence;
    off_t pos;
    json *o;

    if (!h || qga_need_int(args, "offset", &off, err, errlen))
        return 1;
    if (!w)
        return qga_seterr(err, errlen, "Parameter 'whence' is missing");
    if (w->type == J_INT && w->i >= 0 && w->i <= 2)
        whence = (int)w->i;
    else if (w->type == J_STR && strcmp(w->s, "set") == 0)
        whence = SEEK_SET;
    else if (w->type == J_STR && strcmp(w->s, "cur") == 0)
        whence = SEEK_CUR;
    else if (w->type == J_STR && strcmp(w->s, "end") == 0)
        whence = SEEK_END;
    else
        return qga_seterr(err, errlen, "invalid whence");
    pos = lseek(h->fd, (off_t)off, whence);
    if (pos == (off_t)-1)
        return qga_seterr(err, errlen, "failed to seek: %s", strerror(errno));
    o = json_obj();
    json_obj_set(o, "position", json_int((qga_i64)pos));
    json_obj_set(o, "eof", json_bool(0));
    *ret = o;
    return 0;
}

static int cmd_flush(const json *args, json **ret, char *err, size_t errlen)
{
    handle *h = lookup(args, err, errlen);

    (void)ret;
    if (!h)
        return 1;
    if (fsync(h->fd) < 0 && errno != EINVAL && errno != ENOTSUP)
        return qga_seterr(err, errlen, "failed to flush file: %s", strerror(errno));
    return 0;
}

void qga_file_register(void)
{
    qga_register_ex("guest-file-open", cmd_open, QGA_BLOCK_FROZEN);
    qga_register_ex("guest-file-close", cmd_close, QGA_BLOCK_FROZEN);
    qga_register_ex("guest-file-read", cmd_read, QGA_BLOCK_FROZEN);
    qga_register_ex("guest-file-write", cmd_write, QGA_BLOCK_FROZEN);
    qga_register_ex("guest-file-seek", cmd_seek, QGA_BLOCK_FROZEN);
    qga_register_ex("guest-file-flush", cmd_flush, QGA_BLOCK_FROZEN);
}
