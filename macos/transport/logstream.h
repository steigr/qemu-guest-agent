/* logstream.h - the guest's log, followed for the host: /var/log/system.log, where syslogd writes
 * what the system and the kernel log on these releases (the unified log is 10.12's). */
#ifndef QGA_LOGSTREAM_H
#define QGA_LOGSTREAM_H

#include "../core/util.h"

#include <sys/types.h>

#define QGA_LOG_PATH        "/var/log/system.log"
#define QGA_LOG_VSOCK_PORT  7701
#define QGA_LOG_HISTORY     (64 * 1024)    /* replayed to a host that connects */

typedef struct {
    char   path[256];
    ino_t  ino;
    off_t  pos;
    int    open;            /* the file has been seen */
    sbuf   partial;         /* a last line still waiting for its newline */
} ls_tail;

/* Starts at most QGA_LOG_HISTORY before the file's end, at a line's start. */
void ls_tail_init(ls_tail *t, const char *path);
/* Appends the complete lines written since the last call to out; follows a rotation (a new inode, or
 * the file shorter than what was read) from the new file's start. Returns how many bytes. */
size_t ls_tail_poll(ls_tail *t, sbuf *out);
void ls_tail_free(ls_tail *t);

#endif
