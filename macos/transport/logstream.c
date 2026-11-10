/* logstream.c - following /var/log/system.log by polling: stat, then read what is new. */
#include "logstream.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void ls_tail_init(ls_tail *t, const char *path)
{
    struct stat st;

    memset(t, 0, sizeof *t);
    strncpy(t->path, path, sizeof t->path - 1);
    sb_init(&t->partial);
    if (stat(path, &st) == 0) {
        t->ino = st.st_ino;
        t->open = 1;
        t->pos = st.st_size > QGA_LOG_HISTORY ? st.st_size - QGA_LOG_HISTORY : 0;
        if (t->pos > 0)
            t->pos = -t->pos;           /* negative: skip to the next newline first */
    }
}

size_t ls_tail_poll(ls_tail *t, sbuf *out)
{
    struct stat st;
    char buf[8192];
    size_t added = 0;
    int fd, skipping;
    off_t pos;

    if (stat(t->path, &st) != 0)
        return 0;                       /* rotated away, and not created again yet */
    pos = t->pos < 0 ? -t->pos : t->pos;
    if (!t->open || st.st_ino != t->ino || st.st_size < pos) {
        t->ino = st.st_ino;             /* a new file: from its start */
        t->open = 1;
        t->pos = pos = 0;
        sb_reset(&t->partial);
    }
    if (st.st_size == pos)
        return 0;
    fd = open(t->path, O_RDONLY);
    if (fd < 0)
        return 0;
    skipping = t->pos < 0;
    if (lseek(fd, pos, SEEK_SET) == pos) {
        ssize_t n;

        while ((n = read(fd, buf, sizeof buf)) > 0) {
            ssize_t k, start = 0;

            pos += n;
            if (skipping) {
                for (k = 0; k < n && buf[k] != '\n'; k++)
                    ;
                if (k == n)
                    continue;
                start = k + 1;
                skipping = 0;
            }
            for (k = start; k < n; k++) {
                if (buf[k] != '\n')
                    continue;
                if (t->partial.len) {
                    sb_append(out, t->partial.p, t->partial.len);
                    added += t->partial.len;
                    sb_reset(&t->partial);
                }
                sb_append(out, buf + start, (size_t)(k + 1 - start));
                added += (size_t)(k + 1 - start);
                start = k + 1;
            }
            if (start < n)
                sb_append(&t->partial, buf + start, (size_t)(n - start));
            if (pos - (t->pos < 0 ? -t->pos : t->pos) > 4 * QGA_LOG_HISTORY)
                break;                  /* a flood: the rest next time */
        }
    }
    close(fd);
    t->pos = skipping ? -pos : pos;
    return added;
}

void ls_tail_free(ls_tail *t)
{
    sb_free(&t->partial);
}
