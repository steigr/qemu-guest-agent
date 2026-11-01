/* user.c - guest-set-user-password.
 *
 * IRIX's passwd(1) only talks to a terminal, so the hash is written directly: to /etc/shadow if the
 * system has run pwconv and the user is there, otherwise to /etc/passwd. crypt() is DES, so only
 * the first 8 characters of a password count. /etc/ptmp is both the temporary file and the lock. */
#include "../core/base64.h"
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PTMP "/etc/ptmp"
#define MAX_FILE (1024 * 1024)

static void random_salt(char *salt)
{
    static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char r[2] = { 0, 0 };
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        read(fd, r, 2);
        close(fd);
    }
    salt[0] = alphabet[r[0] & 63];
    salt[1] = alphabet[r[1] & 63];
    salt[2] = '\0';
}

static int valid_name(const char *u)
{
    if (!*u || strlen(u) > 64)
        return 0;
    for (; *u; u++)
        if (!((*u >= 'a' && *u <= 'z') || (*u >= 'A' && *u <= 'Z') || (*u >= '0' && *u <= '9') ||
              *u == '.' || *u == '_' || *u == '-'))
            return 0;
    return 1;
}

/* Rewrites PATH with USER's second field set to HASH. Returns 0, 1 with err set, or -1 if the user
 * is not in the file (nothing is changed then). */
static int set_field(const char *path, const char *user, const char *hash, char *err, size_t errlen)
{
    struct stat st;
    size_t ulen = strlen(user), n;
    int found = 0, fd;
    FILE *in, *out;
    char *buf, *p, *e;

    if (stat(path, &st) != 0 || !(in = fopen(path, "r")))
        return qga_seterr(err, errlen, "cannot read %s: %s", path, strerror(errno));
    buf = (char *)qga_xmalloc(MAX_FILE + 1);
    n = fread(buf, 1, MAX_FILE, in);
    fclose(in);
    buf[n] = '\0';
    if (n == MAX_FILE) {
        free(buf);
        return qga_seterr(err, errlen, "%s is too large", path);
    }
    fd = open(PTMP, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
    if (fd < 0) {
        int e0 = errno;

        free(buf);
        return qga_seterr(err, errlen, e0 == EEXIST ? "%s exists: the password file is being edited"
                                                    : "cannot create %s", PTMP);
    }
    out = fdopen(fd, "w");
    for (p = buf; *p; p = e) {
        char *colon;

        e = strchr(p, '\n');
        e = e ? e + 1 : p + strlen(p);
        if (strncmp(p, user, ulen) == 0 && p[ulen] == ':' &&
            (colon = strchr(p + ulen + 1, ':')) != NULL && colon < e) {
            fprintf(out, "%s:%s", user, hash);
            fwrite(colon, 1, (size_t)(e - colon), out);
            found = 1;
        } else {
            fwrite(p, 1, (size_t)(e - p), out);
        }
    }
    free(buf);
    if (!found) {
        fclose(out);
        unlink(PTMP);
        return -1;
    }
    if (fflush(out) != 0 || fsync(fileno(out)) != 0 || fclose(out) != 0 ||
        chown(PTMP, st.st_uid, st.st_gid) != 0 || rename(PTMP, path) != 0) {
        int e1 = errno;

        unlink(PTMP);
        return qga_seterr(err, errlen, "cannot write %s: %s", path, strerror(e1));
    }
    return 0;
}

static int cmd_set_password(const json *args, json **ret, char *err, size_t errlen)
{
    const char *user = NULL, *b64 = NULL, *hash;
    int crypted = 0, rc = -1;
    sbuf pw;
    char salt[3];

    (void)ret;
    if (qga_need_str(args, "username", &user, err, errlen) ||
        qga_need_str(args, "password", &b64, err, errlen) ||
        qga_opt_bool(args, "crypted", &crypted, err, errlen))
        return 1;
    if (!valid_name(user) || !getpwnam(user))
        return qga_seterr(err, errlen, "no such user '%s'", user);
    sb_init(&pw);
    if (base64_decode(b64, strlen(b64), &pw) != 0 || memchr(pw.p ? pw.p : "", '\0', pw.len)) {
        sb_free(&pw);
        return qga_seterr(err, errlen, "invalid base64 in 'password'");
    }
    hash = sb_cstr(&pw);
    if (crypted) {
        if (strchr(hash, ':') || strchr(hash, '\n')) {
            sb_free(&pw);
            return qga_seterr(err, errlen, "invalid crypted password");
        }
    } else {
        random_salt(salt);
        hash = crypt(hash, salt);
        if (!hash) {
            sb_free(&pw);
            return qga_seterr(err, errlen, "crypt failed");
        }
    }
    if (access("/etc/shadow", F_OK) == 0)
        rc = set_field("/etc/shadow", user, hash, err, errlen);
    if (rc < 0)
        rc = set_field("/etc/passwd", user, hash, err, errlen);
    memset(pw.p, 0, pw.len);
    sb_free(&pw);
    if (rc < 0)
        return qga_seterr(err, errlen, "user '%s' is not in the password file", user);
    return rc;
}

void qga_user_register(void)
{
    qga_register_ex("guest-set-user-password", cmd_set_password, QGA_BLOCK_FROZEN);
}
