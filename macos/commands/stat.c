/* stat.c - stand-in for /usr/bin/stat, which 10.0/10.1 do not ship.
 *
 * guest-exec (commands/exec.c) re-executes this binary as `--applet stat ...` when asked to run
 * a missing /usr/bin/stat. Understands the two spellings the host's Stat() uses:
 *   BSD:  stat -f "%z %p %m" PATH     (size, octal st_mode, mtime)
 *   GNU:  stat -c "%s %f %Y" PATH     (size, hex st_mode, mtime)
 * Unknown directives are printed verbatim, like the real tools do not, but never silently wrong. */
#include "../core/dispatch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static void emit(const char *fmt, int gnu, const char *path, const struct stat *st)
{
    const char *p;

    for (p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) {
            putchar(*p);
            continue;
        }
        p++;
        switch (*p) {
        case '%': putchar('%'); break;
        case 'z': if (!gnu) { printf("%lld", (long long)st->st_size); break; } goto verbatim;
        case 's': if (gnu) { printf("%lld", (long long)st->st_size); break; } goto verbatim;
        case 'p': if (!gnu) { printf("%o", (unsigned)st->st_mode); break; } goto verbatim;
        case 'f': if (gnu) { printf("%x", (unsigned)st->st_mode); break; } goto verbatim;
        case 'a': if (gnu) printf("%o", (unsigned)(st->st_mode & 07777));
                  else printf("%ld", (long)st->st_atime);
                  break;
        case 'm': if (!gnu) { printf("%ld", (long)st->st_mtime); break; } goto verbatim;
        case 'c': if (!gnu) { printf("%ld", (long)st->st_ctime); break; } goto verbatim;
        case 'X': if (gnu) { printf("%ld", (long)st->st_atime); break; } goto verbatim;
        case 'Y': if (gnu) { printf("%ld", (long)st->st_mtime); break; } goto verbatim;
        case 'Z': if (gnu) { printf("%ld", (long)st->st_ctime); break; } goto verbatim;
        case 'u': printf("%u", (unsigned)st->st_uid); break;
        case 'g': printf("%u", (unsigned)st->st_gid); break;
        case 'i': printf("%lu", (unsigned long)st->st_ino); break;
        case 'N': case 'n': printf("%s", path); break;
        default:
        verbatim:
            putchar('%');
            putchar(*p);
        }
    }
    putchar('\n');
}

int qga_applet_stat(int argc, char **argv)
{
    const char *fmt = NULL;
    int gnu = 0, k, status = 0, npaths = 0;

    for (k = 0; k < argc; k++) {
        if ((strcmp(argv[k], "-f") == 0 || strcmp(argv[k], "-c") == 0) && k + 1 < argc) {
            gnu = argv[k][1] == 'c';
            fmt = argv[++k];
        } else if (strcmp(argv[k], "-L") == 0) {
            continue;                   /* stat() already follows symlinks */
        } else {
            struct stat st;

            npaths++;
            if (stat(argv[k], &st) != 0) {
                fprintf(stderr, "stat: %s: stat: %s\n", argv[k], strerror(errno));
                status = 1;
                continue;
            }
            emit(fmt ? fmt : "%N: %z bytes, mode %p, mtime %m", fmt ? gnu : 0, argv[k], &st);
        }
    }
    if (!npaths) {
        fprintf(stderr, "usage: stat [-f|-c format] file ...\n");
        return 1;
    }
    return status;
}
