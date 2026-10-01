#include "tty.h"

#include "../core/dispatch.h"
#include "../core/util.h"

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#define MAX_LINE (256 * 1024)
#define DELIM 0xFF

int tty_open(const char *path, channel *ch)
{
    struct termios t;
    int fd, fl;

    if (strcmp(path, "-") == 0) {
        ch->rfd = 0;
        ch->wfd = 1;
        return 0;
    }
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (tcgetattr(fd, &t) == 0) {
        /* Hand-rolled cfmakeraw(): not guaranteed on the oldest releases. */
        t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                       ICRNL | IXON | IXOFF | IXANY);
        t.c_oflag &= ~OPOST;
        t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        t.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
        t.c_cflag |= CS8 | CLOCAL | CREAD;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);     /* spawned children must not inherit the channel */
    fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    ch->rfd = ch->wfd = fd;
    return 0;
}

void tty_close(channel *ch)
{
    if (ch->rfd > 2)
        close(ch->rfd);
    ch->rfd = ch->wfd = -1;
}

int tty_discover(char *out, size_t outlen)
{
    static const char prefix[] = "cu.virtio-";
    DIR *d = opendir("/dev");
    struct dirent *e;
    int count = 0;
    char first[256];

    if (!d)
        return -1;
    first[0] = '\0';
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, sizeof prefix - 1) != 0)
            continue;
        if (count == 0) {
            strncpy(first, e->d_name, sizeof first - 1);
            first[sizeof first - 1] = '\0';
        }
        count++;
        if (count > 1)
            qga_log("candidate: /dev/%s", e->d_name);
    }
    closedir(d);
    if (count == 0)
        return -1;
    if (count > 1) {
        qga_log("candidate: /dev/%s", first);
        return -2;
    }
    if (strlen(first) + 6 > outlen)
        return -1;
    strcpy(out, "/dev/");
    strcat(out, first);
    return 0;
}

void channel_write(const channel *ch, const char *buf, size_t len)
{
    int stalls = 0;

    while (len > 0) {
        ssize_t n = write(ch->wfd, buf, len);

        if (n > 0) {
            buf += n;
            len -= (size_t)n;
            stalls = 0;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set w;
            struct timeval tv;

            if (++stalls > 3) {
                qga_log("write stalled, dropping %lu bytes", (unsigned long)len);
                return;
            }
            FD_ZERO(&w);
            FD_SET(ch->wfd, &w);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            select(ch->wfd + 1, NULL, &w, NULL, &tv);
            continue;
        }
        qga_log("write failed: %s", strerror(errno));
        return;
    }
}

static void process_line(const channel *ch, const char *line, size_t len, sbuf *out)
{
    size_t s = 0;

    while (s < len && (line[s] == ' ' || line[s] == '\t' || line[s] == '\r'))
        s++;
    if (s == len)
        return;                         /* blank line */
    if (qga_verbose)
        qga_log("<- %.*s", (int)(len - s > 300 ? 300 : len - s), line + s);
    sb_reset(out);
    qga_handle(line + s, len - s, out);
    if (qga_verbose)
        qga_log("-> %.*s", (int)(out->len > 300 ? 300 : out->len), out->p);
    if (out->len > 0)
        channel_write(ch, out->p, out->len);
}

int channel_serve(const channel *ch, int is_stdio)
{
    sbuf line, out;
    int discarding = !is_stdio;
    int result = 1;
    unsigned char buf[4096];

    sb_init(&line);
    sb_init(&out);

    for (;;) {
        fd_set r;
        struct timeval tv;
        ssize_t n, k;
        int rc;

        int maxfd = ch->rfd;
        fd_set w;

        FD_ZERO(&r);
        FD_ZERO(&w);
        FD_SET(ch->rfd, &r);
        qga_poll_fill(&r, &w, &maxfd);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        rc = select(maxfd + 1, &r, &w, NULL, &tv);
        if (rc < 0 && errno != EINTR) {
            qga_log("select failed: %s", strerror(errno));
            break;
        }
        qga_poll_service();             /* child I/O and reaping, also on timeout */
        if (rc <= 0 || !FD_ISSET(ch->rfd, &r))
            continue;

        n = read(ch->rfd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK)
                continue;
            qga_log("read failed: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            if (is_stdio)
                result = 0;
            else
                qga_log("channel returned EOF");
            break;
        }

        for (k = 0; k < n; k++) {
            unsigned char c = buf[k];

            if (c == DELIM) {           /* peer resync: drop partial input */
                sb_reset(&line);
                discarding = 0;
            } else if (c == '\n') {
                if (!discarding)
                    process_line(ch, line.p ? line.p : "", line.len, &out);
                sb_reset(&line);
                discarding = 0;
            } else if (!discarding) {
                if (line.len >= MAX_LINE) {
                    qga_log("request line too long, discarding");
                    sb_reset(&line);
                    discarding = 1;
                } else {
                    sb_putc(&line, c);
                }
            }
        }
    }

    sb_free(&line);
    sb_free(&out);
    return result;
}
