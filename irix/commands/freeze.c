/* freeze.c - guest-fsfreeze-*. xfs_freeze freezes an XFS filesystem, but it refuses the root
 * filesystem, so what this gives depends on what is mounted: every other local read-write XFS is
 * really frozen; the root filesystem and any other local one are only synced after the optional
 * hook. Either way the agent refuses file/exec/time/shutdown commands until thaw, so an
 * orchestrator's snapshot window is not disturbed by us. */
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HOOK "/etc/qemu/fsfreeze-hook"
#define XFS_FREEZE "/usr/sbin/xfs_freeze"
#define MAX_FROZEN 32

static int frozen_count = 0;
static char *frozen[MAX_FROZEN];
static int nfrozen = 0;

static int run_quiet(const char *path, const char *a1, const char *a2)
{
    pid_t pid;
    int st;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);

        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        execl(path, path, a1, a2, (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void run_hook(const char *verb)
{
    if (access(HOOK, X_OK) == 0)
        run_quiet(HOOK, verb, NULL);
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

static void thaw_all(void)
{
    while (nfrozen > 0) {
        nfrozen--;
        if (run_quiet(XFS_FREEZE, "-u", frozen[nfrozen]) != 0)
            qga_log("%s -u %s failed", XFS_FREEZE, frozen[nfrozen]);
        free(frozen[nfrozen]);
    }
}

static int do_freeze(const json *list, json **ret, char *err, size_t errlen)
{
    FILE *fp;
    struct mntent *m;
    int count = 0;

    if (qga_frozen)
        return qga_seterr(err, errlen, "the filesystems are already frozen");
    fp = setmntent(MOUNTED, "r");
    if (!fp)
        return qga_seterr(err, errlen, "cannot read %s: %s", MOUNTED, strerror(errno));
    run_hook("freeze");
    sync();
    while ((m = getmntent(fp)) != NULL) {
        int xfs = strcmp(m->mnt_type, "xfs") == 0;

        if ((!xfs && strcmp(m->mnt_type, "efs") != 0) || hasmntopt(m, MNTOPT_RO) ||
            !selected(list, m->mnt_dir))
            continue;
        if (xfs && strcmp(m->mnt_dir, "/") != 0) {
            if (nfrozen == MAX_FROZEN || run_quiet(XFS_FREEZE, "-f", m->mnt_dir) != 0) {
                qga_seterr(err, errlen, "cannot freeze %s", m->mnt_dir);
                endmntent(fp);
                thaw_all();
                run_hook("thaw");
                return 1;
            }
            frozen[nfrozen++] = qga_xstrdup(m->mnt_dir);
        }
        count++;
    }
    endmntent(fp);
    qga_log_pause(1);
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
    if (qga_frozen) {
        thaw_all();
        qga_log_pause(0);
        run_hook("thaw");
    }
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
