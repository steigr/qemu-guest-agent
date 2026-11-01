/* qemu-guest-agent for IRIX 6.5: the QEMU guest agent protocol over a vioser port. */
#include "core/dispatch.h"
#include "core/util.h"
#include "install.h"
#include "selftest.h"
#include "transport/serve.h"
#include "transport/logstream.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char qga_self_path[1024];

/* Absolute path of this binary: argv[0] made absolute, or searched on PATH. Lookups are done
 * once at startup, before any chdir. */
static void find_self(const char *argv0)
{
    qga_self_path[0] = '\0';
    if (strchr(argv0, '/')) {
        if (argv0[0] == '/') {
            strncpy(qga_self_path, argv0, sizeof qga_self_path - 1);
        } else if (getcwd(qga_self_path, sizeof qga_self_path - 2)) {
            strcat(qga_self_path, "/");
            strncat(qga_self_path, argv0[0] == '.' && argv0[1] == '/' ? argv0 + 2 : argv0,
                    sizeof qga_self_path - strlen(qga_self_path) - 1);
        }
    } else {
        const char *path = getenv("PATH");
        char *copy, *dir;

        if (!path)
            return;
        copy = qga_xstrdup(path);
        for (dir = strtok(copy, ":"); dir; dir = strtok(NULL, ":")) {
            char cand[1024];
            struct stat st;

            if (strlen(dir) + strlen(argv0) + 2 > sizeof cand)
                continue;
            sprintf(cand, "%s/%s", dir, argv0);
            if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
                strcpy(qga_self_path, cand);
                break;
            }
        }
        free(copy);
    }
    qga_self_path[sizeof qga_self_path - 1] = '\0';
}

static void usage(void)
{
    puts("usage: qemu-guest-agent [--tty PATH|auto|-] [--log FILE]\n"
         "                        [--log-stream auto|off] [--log-source FILE]\n"
         "                        [--daemon [--pidfile FILE]]\n"
         "                        [--verbose] [--strict-exec] [--self-test] [--version]\n"
         "       qemu-guest-agent --install | --upgrade | --uninstall\n"
         "  --tty PATH   port or serial node to serve (default: auto = /hw/virtio-ports/\n"
         "               org.qemu.guest_agent.0, or org.iris.agent.0)\n"
         "               '-' serves stdin/stdout, for testing\n"
         "  --log FILE   append log messages to FILE instead of stderr\n"
         "  --log-stream auto|off  ship the guest's log to the host (default auto: new lines of\n"
         "               /var/adm/SYSLOG on /hw/virtio-ports/org.iris.log.0, if there is one)\n"
         "  --log-source FILE  the file the log stream follows\n"
         "  --verbose    log every request and response\n"
         "  --strict-exec  a guest-exec whose binary cannot be run fails the request (upstream qemu-ga);\n"
         "               default: the child exits 127 with the reason on stderr");
}

int main(int argc, char **argv)
{
    const char *tty = "auto";
    int log_stream = 1;
    const char *log_source = QGA_LOG_PATH;
    qga_serve_cfg cfg;
    const char *pidfile = QGA_PIDFILE;
    int k, daemonize = 0, have_log = 0;

    find_self(argv[0]);
    if (argc >= 4 && strcmp(argv[1], "--applet") == 0 && strcmp(argv[2], "stat") == 0)
        return qga_applet_stat(argc - 3, argv + 3);

    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--tty") == 0 && k + 1 < argc) {
            tty = argv[++k];
        } else if (strcmp(argv[k], "--log-stream") == 0 && k + 1 < argc) {
            log_stream = strcmp(argv[++k], "off") != 0;
        } else if (strcmp(argv[k], "--log-source") == 0 && k + 1 < argc) {
            log_source = argv[++k];
        } else if (strcmp(argv[k], "--log") == 0 && k + 1 < argc) {
            qga_log_open(argv[++k]);
            have_log = 1;
        } else if (strcmp(argv[k], "--daemon") == 0) {
            daemonize = 1;
        } else if (strcmp(argv[k], "--pidfile") == 0 && k + 1 < argc) {
            pidfile = argv[++k];
        } else if (strcmp(argv[k], "--install") == 0 || strcmp(argv[k], "--upgrade") == 0) {
            return qga_install();
        } else if (strcmp(argv[k], "--uninstall") == 0) {
            return qga_uninstall();
        } else if (strcmp(argv[k], "--verbose") == 0) {
            qga_verbose = 1;
        } else if (strcmp(argv[k], "--strict-exec") == 0) {
            qga_exec_strict = 1;
        } else if (strcmp(argv[k], "--self-test") == 0) {
            return qga_selftest();
        } else if (strcmp(argv[k], "--version") == 0) {
            puts(QGA_VERSION);
            return 0;
        } else {
            usage();
            return strcmp(argv[k], "--help") == 0 || strcmp(argv[k], "-h") == 0 ? 0 : 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    if (daemonize) {
        if (!have_log)
            qga_log_open(QGA_DEFAULT_LOG);
        if (qga_daemonize(pidfile) != 0) {
            qga_log("daemonize failed");
            return 1;
        }
    }
    qga_protocol_register();
    qga_file_register();
    qga_hw_register();
    qga_freeze_register();
    qga_ssh_register();
    qga_user_register();
    qga_exec_register();
    qga_system_register();
    /* rc scripts hand us a minimal environment. */
    putenv("PATH=/usr/sbin:/usr/bsd:/sbin:/usr/bin:/etc:/usr/etc:/usr/local/bin");

    if (strcmp(tty, "-") == 0)
        log_stream = 0;                 /* stdin/stdout is a test of one channel */
    cfg.tty = tty;
    cfg.log_path = log_stream ? log_source : NULL;
    cfg.log_tty = log_stream ? "auto" : NULL;
    return qga_serve(&cfg);
}
