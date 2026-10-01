/* freeze.c - guest-fsfreeze-*. HFS+ has no kernel freeze (plan 5.3), so this is the honest
 * subset: run the optional hook, sync, and F_FULLFSYNC each local filesystem's root. The guest
 * is NOT quiesced afterwards; what "frozen" adds is that this agent refuses file/exec/time/
 * shutdown commands until thaw, so an orchestrator's snapshot window is not disturbed by us. */
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef F_FULLFSYNC
#define F_FULLFSYNC 51      /* not in the 10.0/10.1 headers; EINVAL at runtime if unsupported */
#endif

#define HOOK "/etc/qemu/fsfreeze-hook"

static int frozen_count = 0;

static void run_hook(const char *verb)
{
    pid_t pid;
    int st;

    if (access(HOOK, X_OK) != 0)
        return;
    pid = fork();
    if (pid == 0) {
        execl(HOOK, HOOK, verb, (char *)NULL);
        _exit(127);
    }
    if (pid > 0)
        waitpid(pid, &st, 0);
}

static int selected(const json *list, const char *mnt)
{
    size_t k;

    if (!list)
        return 1;
    for (k = 0; k < list->n; k++)
        if (list->items[k]->type == J_STR && strcmp(list->items[k]->s, mnt) == 0)
            return 1;
    return 0;
}

static int do_freeze(const json *list, json **ret, char *err, size_t errlen)
{
    struct statfs *fs;
    int n, k, count = 0;

    if (qga_frozen)
        return qga_seterr(err, errlen, "the filesystems are already frozen");
    n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n < 0)
        return qga_seterr(err, errlen, "getfsstat failed: %s", strerror(errno));
    fs = (struct statfs *)qga_xmalloc((size_t)(n + 1) * sizeof *fs);
    n = getfsstat(fs, (long)((n + 1) * sizeof *fs), MNT_NOWAIT);

    run_hook("freeze");
    sync();
    for (k = 0; k < n; k++) {
        int fd;

        if (!(fs[k].f_flags & MNT_LOCAL) || (fs[k].f_flags & MNT_RDONLY) ||
            !selected(list, fs[k].f_mntonname))
            continue;
        fd = open(fs[k].f_mntonname, O_RDONLY);
        if (fd < 0)
            continue;
        fcntl(fd, F_FULLFSYNC, 0);      /* best effort: EINVAL where unsupported */
        close(fd);
        count++;
    }
    free(fs);
    qga_frozen = 1;
    frozen_count = count;
    *ret = json_int(count);
    return 0;
}

static int cmd_freeze(const json *args, json **ret, char *err, size_t errlen)
{
    (void)args;
    return do_freeze(NULL, ret, err, errlen);
}

static int cmd_freeze_list(const json *args, json **ret, char *err, size_t errlen)
{
    const json *l = args ? json_obj_get(args, "mountpoints") : NULL;

    if (l && l->type != J_ARR)
        return qga_seterr(err, errlen, "Invalid parameter type for 'mountpoints', expected: array");
    return do_freeze(l, ret, err, errlen);
}

static int cmd_thaw(const json *args, json **ret, char *err, size_t errlen)
{
    (void)args; (void)err; (void)errlen;
    if (qga_frozen)
        run_hook("thaw");
    *ret = json_int(qga_frozen ? frozen_count : 0);
    qga_frozen = 0;
    frozen_count = 0;
    return 0;
}

static int cmd_status(const json *args, json **ret, char *err, size_t errlen)
{
    (void)args; (void)err; (void)errlen;
    *ret = json_str(qga_frozen ? "frozen" : "thawed");
    return 0;
}

void qga_freeze_register(void)
{
    qga_register("guest-fsfreeze-status", cmd_status);
    qga_register("guest-fsfreeze-freeze", cmd_freeze);
    qga_register("guest-fsfreeze-freeze-list", cmd_freeze_list);
    qga_register("guest-fsfreeze-thaw", cmd_thaw);
}
