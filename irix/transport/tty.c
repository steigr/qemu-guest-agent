#include "tty.h"

#include "../core/dispatch.h"
#include "../core/util.h"

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

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
    /* A vioser port is not a tty, so this is for --tty with a real serial port. */
    if (tcgetattr(fd, &t) == 0) {
        t.c_iflag = 0;
        t.c_oflag = 0;
        t.c_lflag = 0;
        t.c_cflag = (t.c_cflag & CBAUD) | CS8 | CREAD | CLOCAL;
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
        /* What a previous open left unsent is a reply to a host that has given up on it. Output
         * only: the input may already hold the host's resync. */
        tcflush(fd, TCOFLUSH);
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

int tty_discover(int log, char *out, size_t outlen)
{
    /* org.qemu.guest_agent.0 is the name qemu's own hosts look for. */
    static const char *const agent_names[] = { "org.qemu.guest_agent.0", "org.iris.agent.0", NULL };
    static const char *const log_names[] = { "org.iris.log.0", NULL };
    const char *const *n;

    for (n = log ? log_names : agent_names; *n; n++) {
        char path[128];

        sprintf(path, "%s/%s", QGA_PORT_DIR, *n);
        if (strlen(path) < outlen && access(path, F_OK) == 0) {
            strcpy(out, path);
            return 0;
        }
    }
    return -1;
}
