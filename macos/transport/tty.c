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
