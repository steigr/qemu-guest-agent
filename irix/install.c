#include "install.h"

#include "core/dispatch.h"
#include "core/util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BIN_DIR "/usr/local/libexec"
#define BIN BIN_DIR "/qemu-guest-agent"
#define INIT "/etc/init.d/qemu-guest-agent"
#define CONFIG "/etc/config/qemu-guest-agent"
/* vioser's links are S98 and K02: the agent starts after the driver and stops before it, since a
 * driver with an open port does not unload. It would find the port anyway, retrying. */
#define RC_START "/etc/rc2.d/S99qemu-guest-agent"
#define RC_STOP "/etc/rc0.d/K01qemu-guest-agent"

int qga_daemonize(const char *pidfile)
{
    pid_t pid = fork();
    int fd;
    FILE *fp;

    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    setsid();
    chdir("/");
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
    fp = fopen(pidfile, "w");
    if (fp) {
        fprintf(fp, "%d\n", (int)getpid());
        fclose(fp);
    }
    return 0;
}

static void make_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        printf("warning: mkdir %s: %s\n", path, strerror(errno));
}

static int write_file(const char *path, const char *content, mode_t mode)
{
    FILE *fp = fopen(path, "w");

    if (!fp) {
        printf("error: cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fputs(content, fp);
    fclose(fp);
    chmod(path, mode);
    chown(path, 0, 0);
    printf("wrote %s\n", path);
    return 0;
}

static int link_rc(const char *path)
{
    unlink(path);
    if (symlink("../init.d/qemu-guest-agent", path) != 0) {
        printf("error: cannot link %s: %s\n", path, strerror(errno));
        return -1;
    }
    printf("linked %s\n", path);
    return 0;
}

static int copy_self(void)
{
    FILE *in, *out;
    char buf[8192], tmp[64];
    size_t n;

    if (strcmp(qga_self_path, BIN) == 0)
        return 0;
    in = fopen(qga_self_path, "r");
    if (!in) {
        printf("error: cannot read %s\n", qga_self_path);
        return -1;
    }
    make_dir("/usr/local");
    make_dir(BIN_DIR);
    sprintf(tmp, "%s.new", BIN);
    out = fopen(tmp, "w");
    if (!out) {
        printf("error: cannot write %s: %s\n", tmp, strerror(errno));
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    if (fclose(out) != 0 || chmod(tmp, 0755) != 0 || rename(tmp, BIN) != 0) {
        printf("error: installing %s failed: %s\n", BIN, strerror(errno));
        unlink(tmp);
        return -1;
    }
    chown(BIN, 0, 0);
    printf("installed %s\n", BIN);
    return 0;
}

static const char init_script[] =
    "#! /sbin/sh\n"
    "# QEMU guest agent. chkconfig qemu-guest-agent off to keep it from starting.\n"
    "PIDFILE=" QGA_PIDFILE "\n"
    "case \"$1\" in\n"
    "'start')\n"
    "\tif /etc/chkconfig qemu-guest-agent; then\n"
    "\t\t" BIN " --daemon\n"
    "\tfi\n"
    "\t;;\n"
    "'stop')\n"
    "\tif [ -f $PIDFILE ]; then\n"
    "\t\tkill `cat $PIDFILE` 2>/dev/null\n"
    "\t\trm -f $PIDFILE\n"
    "\tfi\n"
    "\t;;\n"
    "*)\n"
    "\techo \"usage: $0 {start|stop}\"\n"
    "\t;;\n"
    "esac\n"
    "exit 0\n";

int qga_install(void)
{
    if (geteuid() != 0) {
        printf("error: --install must run as root\n");
        return 1;
    }
    if (copy_self() != 0)
        return 1;
    if (write_file(INIT, init_script, 0755) != 0 ||
        write_file(CONFIG, "on\n", 0644) != 0 ||
        link_rc(RC_START) != 0 || link_rc(RC_STOP) != 0)
        return 1;
    printf("it starts at the next boot, or run: %s start\n", INIT);
    return 0;
}

int qga_uninstall(void)
{
    if (geteuid() != 0) {
        printf("error: --uninstall must run as root\n");
        return 1;
    }
    unlink(RC_START);
    unlink(RC_STOP);
    unlink(INIT);
    unlink(CONFIG);
    unlink(BIN);
    printf("removed the rc script and %s (a running agent was not stopped)\n", BIN);
    return 0;
}
