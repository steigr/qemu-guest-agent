#include "tty.h"

#include "../core/dispatch.h"
#include "../core/util.h"

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

/* The nth /dev/cu.virtio-N node, counted from the lowest N: with two virtio consoles the first is
 * the agent's and the second the log stream's (the host gives the agent's the lower PCI slot, and
 * VirtioConsole numbers the nodes in the order the devices attach). */
int tty_discover_nth(int nth, char *out, size_t outlen)
{
    static const char prefix[] = "cu.virtio-";
    DIR *d = opendir("/dev");
    struct dirent *e;
    int nums[16], count = 0, k, m;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL && count < 16) {
        char *end;
        long n;

        if (strncmp(e->d_name, prefix, sizeof prefix - 1) != 0)
            continue;
        n = strtol(e->d_name + sizeof prefix - 1, &end, 10);
        if (*end == '\0' && n >= 0)
            nums[count++] = (int)n;
    }
    closedir(d);
    for (k = 1; k < count; k++)         /* a handful: insertion sort */
        for (m = k; m > 0 && nums[m - 1] > nums[m]; m--) {
            int t = nums[m];

            nums[m] = nums[m - 1];
            nums[m - 1] = t;
        }
    if (nth >= count || outlen < sizeof "/dev/cu.virtio-" + 12)
        return -1;
    sprintf(out, "/dev/cu.virtio-%d", nums[nth]);
    return 0;
}

int tty_discover(char *out, size_t outlen)
{
    return tty_discover_nth(0, out, outlen);
}
