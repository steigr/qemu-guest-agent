/* user.c - guest-set-user-password.
 *
 * 10.3+: `dscl . -passwd` (plain text only). Before that the user database is NetInfo: the
 * crypt(3) hash is stored with `niutil -createprop`. crypt() is DES, so only the first 8
 * characters of the password count there. */
#include "../core/base64.h"
#include "../core/dispatch.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int run(char *const argv[])
{
    pid_t pid = fork();
    int st;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);

        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

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

static int cmd_set_password(const json *args, json **ret, char *err, size_t errlen)
{
    const char *user = NULL, *b64 = NULL;
    int crypted = 0, rc;
    sbuf pw;
    char node[100], salt[3];
    char *argv[8];

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

    if (access("/usr/bin/dscl", X_OK) == 0) {
        if (crypted) {
            sb_free(&pw);
            return qga_seterr(err, errlen, "crypted passwords are not supported with dscl");
        }
        sprintf(node, "/Users/%s", user);
        argv[0] = "/usr/bin/dscl";
        argv[1] = ".";
        argv[2] = "-passwd";
        argv[3] = node;
        argv[4] = (char *)sb_cstr(&pw);
        argv[5] = NULL;
    } else {
        const char *hash = sb_cstr(&pw);

        if (!crypted) {
            random_salt(salt);
            hash = crypt(hash, salt);
            if (!hash) {
                sb_free(&pw);
                return qga_seterr(err, errlen, "crypt failed");
            }
        }
        sprintf(node, "/users/%s", user);
        argv[0] = "/usr/bin/niutil";
        argv[1] = "-createprop";
        argv[2] = ".";
        argv[3] = node;
        argv[4] = "passwd";
        argv[5] = (char *)hash;
        argv[6] = NULL;
    }
    rc = run(argv);
    memset(pw.p ? pw.p : "", 0, pw.len);
    sb_free(&pw);
    if (rc != 0)
        return qga_seterr(err, errlen, "failed to set the password: %s exited with %d", argv[0], rc);
    return 0;
}

void qga_user_register(void)
{
    qga_register_ex("guest-set-user-password", cmd_set_password, QGA_BLOCK_FROZEN);
}
