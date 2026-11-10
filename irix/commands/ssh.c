/* ssh.c - guest-ssh-get/add/remove-authorized-keys: edits ~USER/.ssh/authorized_keys.
 * The file is rewritten through a temp file and renamed, owned by the user, mode 0600. */
#include "../core/dispatch.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FILE (1024 * 1024)

typedef struct {
    char **line;
    size_t n;
} lines;

static void lines_add(lines *l, const char *s, size_t len)
{
    l->line = (char **)qga_xrealloc(l->line, (l->n + 1) * sizeof *l->line);
    l->line[l->n++] = qga_xstrndup(s, len);
}

static void lines_free(lines *l)
{
    size_t k;

    for (k = 0; k < l->n; k++)
        free(l->line[k]);
    free(l->line);
    l->line = NULL;
    l->n = 0;
}

static int lines_has(const lines *l, const char *s)
{
    size_t k;

    for (k = 0; k < l->n; k++)
        if (strcmp(l->line[k], s) == 0)
            return 1;
    return 0;
}

/* Reads the file as trimmed non-empty non-comment lines. A missing file is an empty list. */
static int read_keys(const char *path, lines *out, char *err, size_t errlen)
{
    FILE *fp = fopen(path, "r");
    char *buf, *p, *e;
    size_t n;

    out->line = NULL;
    out->n = 0;
    if (!fp) {
        if (errno == ENOENT)
            return 0;
        return qga_seterr(err, errlen, "failed to open %s: %s", path, strerror(errno));
    }
    buf = (char *)qga_xmalloc(MAX_FILE + 1);
    n = fread(buf, 1, MAX_FILE, fp);
    fclose(fp);
    buf[n] = '\0';
    for (p = buf; *p; p = e) {
        char *s = p, *t;

        e = strchr(p, '\n');
        t = e ? e : p + strlen(p);
        e = e ? e + 1 : t;
        while (s < t && (*s == ' ' || *s == '\t'))
            s++;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r'))
            t--;
        if (t > s && *s != '#')
            lines_add(out, s, (size_t)(t - s));
    }
    free(buf);
    return 0;
}

static int write_keys(const char *dir, const char *path, const lines *l, struct passwd *pw,
                      char *err, size_t errlen)
{
    char tmp[1100];
    FILE *fp;
    int fd;
    size_t k;

    mkdir(dir, 0700);
    chown(dir, pw->pw_uid, pw->pw_gid);
    sprintf(tmp, "%.1000s.tmp.%d", path, (int)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return qga_seterr(err, errlen, "failed to create %s: %s", tmp, strerror(errno));
    fp = fdopen(fd, "w");
    for (k = 0; k < l->n; k++)
        fprintf(fp, "%s\n", l->line[k]);
    if (fclose(fp) != 0 || chown(tmp, pw->pw_uid, pw->pw_gid) != 0 || rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        return qga_seterr(err, errlen, "failed to write %s: %s", path, strerror(e));
    }
    return 0;
}

static int setup(const json *args, struct passwd **pw, char *dir, char *path, char *err, size_t errlen)
{
    const char *user = NULL;

    if (qga_need_str(args, "username", &user, err, errlen))
        return 1;
    *pw = getpwnam(user);
    if (!*pw)
        return qga_seterr(err, errlen, "failed to get passwd entry for user '%s'", user);
    if (strlen((*pw)->pw_dir) > 900)
        return qga_seterr(err, errlen, "home directory path too long");
    sprintf(dir, "%s/.ssh", (*pw)->pw_dir);
    sprintf(path, "%s/authorized_keys", dir);
    return 0;
}

static int key_list(const json *args, lines *keys, char *err, size_t errlen)
{
    const json *a = json_obj_get(args, "keys");
    size_t k;

    keys->line = NULL;
    keys->n = 0;
    if (!a)
        return qga_seterr(err, errlen, "Parameter 'keys' is missing");
    if (a->type != J_ARR)
        return qga_seterr(err, errlen, "Invalid parameter type for 'keys', expected: array of string");
    for (k = 0; k < a->n; k++) {
        const char *s;

        if (a->items[k]->type != J_STR) {
            lines_free(keys);
            return qga_seterr(err, errlen, "Invalid parameter type for 'keys', expected: array of string");
        }
        s = a->items[k]->s;
        if (!*s || strchr(s, '\n') || strchr(s, '\r')) {
            lines_free(keys);
            return qga_seterr(err, errlen, "invalid key: keys must be single non-empty lines");
        }
        lines_add(keys, s, strlen(s));
    }
    return 0;
}

static int cmd_get(const json *args, json **ret, char *err, size_t errlen)
{
    struct passwd *pw;
    char dir[1000], path[1100];
    lines cur;
    json *o, *a;
    size_t k;

    if (setup(args, &pw, dir, path, err, errlen) || read_keys(path, &cur, err, errlen))
        return 1;
    o = json_obj();
    a = json_arr();
    for (k = 0; k < cur.n; k++)
        json_arr_add(a, json_str(cur.line[k]));
    json_obj_set(o, "keys", a);
    lines_free(&cur);
    *ret = o;
    return 0;
}

static int cmd_add(const json *args, json **ret, char *err, size_t errlen)
{
    struct passwd *pw;
    char dir[1000], path[1100];
    lines cur, keys;
    int reset = 0, rc;
    size_t k;

    (void)ret;
    if (setup(args, &pw, dir, path, err, errlen) || key_list(args, &keys, err, errlen))
        return 1;
    if (qga_opt_bool(args, "reset", &reset, err, errlen)) {
        lines_free(&keys);
        return 1;
    }
    if (reset) {
        cur.line = NULL;
        cur.n = 0;
    } else if (read_keys(path, &cur, err, errlen)) {
        lines_free(&keys);
        return 1;
    }
    for (k = 0; k < keys.n; k++)
        if (!lines_has(&cur, keys.line[k]))
            lines_add(&cur, keys.line[k], strlen(keys.line[k]));
    rc = write_keys(dir, path, &cur, pw, err, errlen);
    lines_free(&cur);
    lines_free(&keys);
    return rc;
}

static int cmd_remove(const json *args, json **ret, char *err, size_t errlen)
{
    struct passwd *pw;
    char dir[1000], path[1100];
    lines cur, keys, kept;
    size_t k;
    int rc;

    (void)ret;
    if (setup(args, &pw, dir, path, err, errlen) || key_list(args, &keys, err, errlen))
        return 1;
    if (read_keys(path, &cur, err, errlen)) {
        lines_free(&keys);
        return 1;
    }
    kept.line = NULL;
    kept.n = 0;
    for (k = 0; k < cur.n; k++)
        if (!lines_has(&keys, cur.line[k]))
            lines_add(&kept, cur.line[k], strlen(cur.line[k]));
    rc = cur.n == kept.n ? 0 : write_keys(dir, path, &kept, pw, err, errlen);
    lines_free(&cur);
    lines_free(&keys);
    lines_free(&kept);
    return rc;
}

void qga_ssh_register(void)
{
    qga_register("guest-ssh-get-authorized-keys", cmd_get);
    qga_register_ex("guest-ssh-add-authorized-keys", cmd_add, QGA_BLOCK_FROZEN);
    qga_register_ex("guest-ssh-remove-authorized-keys", cmd_remove, QGA_BLOCK_FROZEN);
}
