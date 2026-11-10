#include "install.h"

#include "core/dispatch.h"
#include "core/util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BIN_DIR "/usr/local/libexec"
#define BIN BIN_DIR "/qemu-guest-agent"
#define ITEM_DIR "/Library/StartupItems/QemuGuestAgent"
#define LD_PLIST "/Library/LaunchDaemons/org.qemu.guest-agent.plist"

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

/* Darwin kernel major version: 5 = 10.1, 6 = 10.2, 7 = 10.3, 8 = 10.4 (launchd). */
static int darwin_major(void)
{
    int mib[2];
    char rel[32];
    size_t len = sizeof rel;

    mib[0] = CTL_KERN;
    mib[1] = KERN_OSRELEASE;
    if (sysctl(mib, 2, rel, &len, NULL, 0) < 0)
        return 0;
    return atoi(rel);
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

/* No RunService/StartService helpers: 10.0-10.3's rc.common lacks them, so dispatch by hand. */
static const char item_script[] =
    "#!/bin/sh\n"
    ". /etc/rc.common\n"
    "PIDFILE=" QGA_PIDFILE "\n"
    "case \"$1\" in\n"
    "stop|restart)\n"
    "    ConsoleMessage \"Stopping the QEMU guest agent\"\n"
    "    [ -f $PIDFILE ] && kill `cat $PIDFILE` && rm -f $PIDFILE\n"
    "    [ \"$1\" = stop ] && exit 0\n"
    "    ;;\n"
    "esac\n"
    "ConsoleMessage \"Starting the QEMU guest agent\"\n"
    BIN " --daemon --tty auto --log " QGA_DEFAULT_LOG "\n";

static const char item_params[] =
    "{\n"
    "  Description     = \"QEMU guest agent\";\n"
    "  Provides        = (\"QEMU Guest Agent\");\n"
    "  Requires        = (\"Disks\");\n"
    "  OrderPreference = \"None\";\n"
    "  Messages =\n"
    "  {\n"
    "    start = \"Starting the QEMU guest agent\";\n"
    "    stop  = \"Stopping the QEMU guest agent\";\n"
    "  };\n"
    "}\n";

static const char daemon_plist[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key><string>org.qemu.guest-agent</string>\n"
    "  <key>ProgramArguments</key>\n"
    "  <array>\n"
    "    <string>" BIN "</string>\n"
    "    <string>--tty</string><string>auto</string>\n"
    "    <string>--log</string><string>" QGA_DEFAULT_LOG "</string>\n"
    "  </array>\n"
    "  <key>RunAtLoad</key><true/>\n"
    "  <key>KeepAlive</key><true/>\n"
    "</dict>\n"
    "</plist>\n";

int qga_install(void)
{
    int startup_item = darwin_major() < 8;

    if (geteuid() != 0) {
        printf("error: --install must run as root\n");
        return 1;
    }
    if (access("/Library/StartupItems/MacGuestAgent", F_OK) == 0)
        printf("warning: /Library/StartupItems/MacGuestAgent (the Perl agent) is installed; both "
               "agents would open the same channel. Remove it or move it aside.\n");
    if (copy_self() != 0)
        return 1;
    if (startup_item) {
        char path[128];

        make_dir("/Library/StartupItems");
        make_dir(ITEM_DIR);
        sprintf(path, "%s/QemuGuestAgent", ITEM_DIR);
        if (write_file(path, item_script, 0755) != 0)
            return 1;
        sprintf(path, "%s/StartupParameters.plist", ITEM_DIR);
        if (write_file(path, item_params, 0644) != 0)
            return 1;
        printf("StartupItem installed; it starts at the next boot, or run: %s/QemuGuestAgent start\n", ITEM_DIR);
    } else {
        if (write_file(LD_PLIST, daemon_plist, 0644) != 0)
            return 1;
        printf("LaunchDaemon installed; it starts at the next boot, or run: launchctl load %s\n", LD_PLIST);
    }
    return 0;
}

int qga_uninstall(void)
{
    char path[128];

    if (geteuid() != 0) {
        printf("error: --uninstall must run as root\n");
        return 1;
    }
    sprintf(path, "%s/QemuGuestAgent", ITEM_DIR);
    unlink(path);
    sprintf(path, "%s/StartupParameters.plist", ITEM_DIR);
    unlink(path);
    rmdir(ITEM_DIR);
    unlink(LD_PLIST);
    unlink(BIN);
    printf("removed the StartupItem / LaunchDaemon and %s (a running agent was not stopped)\n", BIN);
    return 0;
}
