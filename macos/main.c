/* qemu-guest-agent for PowerPC Mac OS X - see doc/plans/ppc-macosx-qga-implementation-plan.md */
#include "core/dispatch.h"
#include "core/util.h"
#include "selftest.h"
#include "transport/tty.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RETRY_SECONDS 2

static void usage(void)
{
    puts("usage: qemu-guest-agent [--tty PATH|auto|-] [--log FILE] [--verbose] [--strict-exec] [--self-test] [--version]\n"
         "  --tty PATH   serial node to serve (default: auto = the only /dev/cu.virtio-*)\n"
         "               '-' serves stdin/stdout, for testing\n"
         "  --log FILE   append log messages to FILE instead of stderr\n"
         "  --verbose    log every request and response\n"
         "  --strict-exec  a guest-exec whose binary cannot be run fails the request (upstream qemu-ga);\n"
         "               default: the child exits 127 with the reason on stderr (Perl agent)");
}

int main(int argc, char **argv)
{
    const char *tty = "auto";
    int k;

    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--tty") == 0 && k + 1 < argc) {
            tty = argv[++k];
        } else if (strcmp(argv[k], "--log") == 0 && k + 1 < argc) {
            qga_log_open(argv[++k]);
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
    qga_protocol_register();
    qga_file_register();
    qga_exec_register();
    qga_system_register();
    /* Service wrappers hand us a minimal environment (plan section 6). */
    putenv("PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin");

    /* Reconnect forever: the channel can vanish on kext reload or VM restore,
     * and node names may change, so discovery repeats after every loss. */
    for (;;) {
        char path[256];
        channel ch;
        int stdio = strcmp(tty, "-") == 0;

        if (strcmp(tty, "auto") == 0) {
            int rc = tty_discover(path, sizeof path);

            if (rc == -2)
                qga_log("several /dev/cu.virtio-* nodes; pin one with --tty");
            else if (rc < 0)
                qga_log("no /dev/cu.virtio-* node yet");
            if (rc != 0) {
                sleep(RETRY_SECONDS);
                continue;
            }
        } else {
            strncpy(path, tty, sizeof path - 1);
            path[sizeof path - 1] = '\0';
        }

        if (tty_open(path, &ch) != 0) {
            qga_log("cannot open %s", path);
            sleep(RETRY_SECONDS);
            continue;
        }
        qga_log("serving %s", path);
        if (channel_serve(&ch, stdio) == 0) {
            tty_close(&ch);
            return 0;
        }
        tty_close(&ch);
        qga_log("channel lost, reconnecting");
        sleep(RETRY_SECONDS);
    }
}
