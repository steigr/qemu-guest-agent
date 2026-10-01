/* exec.c - guest-exec / guest-exec-status: fork + execvp, pipe capture, waitpid.
 *
 * Children run asynchronously. Their pipes are non-blocking and are serviced from the
 * agent's select loop (qga_poller), so a chatty child never stalls on a full pipe. */
#include "../core/base64.h"
#include "../core/dispatch.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define MAX_PROCS 32
#define CAPTURE_MAX (1024 * 1024)   /* per stream; excess is discarded and flagged */

extern char **environ;

typedef struct {
    int used;
    pid_t pid;
    int reaped;
    int status;
    int capture;
    int in_fd, out_fd, err_fd;      /* parent ends, -1 when closed */
    sbuf in;
    size_t in_off;
    sbuf out, err;
    int out_trunc, err_trunc;
} proc;

static proc procs[MAX_PROCS];

int qga_exec_strict = 0;   /* --strict-exec: upstream qemu-ga semantics, fail guest-exec itself */

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void drain(int *fd, sbuf *b, int *trunc)
{
    char buf[4096];

    while (*fd >= 0) {
        ssize_t n = read(*fd, buf, sizeof buf);

        if (n > 0) {
            size_t room = b->len < CAPTURE_MAX ? CAPTURE_MAX - b->len : 0;
            size_t take = (size_t)n < room ? (size_t)n : room;

            if (take)
                sb_append(b, buf, take);
            if (take < (size_t)n)
                *trunc = 1;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        close_fd(fd);               /* EOF or error */
    }
}

static void feed(proc *p)
{
    while (p->in_fd >= 0) {
        ssize_t n;

        if (p->in_off >= p->in.len) {
            close_fd(&p->in_fd);
            return;
        }
        n = write(p->in_fd, p->in.p + p->in_off, p->in.len - p->in_off);
        if (n > 0) {
            p->in_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return;
        close_fd(&p->in_fd);        /* EPIPE: child closed its stdin */
    }
}

static void service_proc(proc *p)
{
    feed(p);
    drain(&p->out_fd, &p->out, &p->out_trunc);
    drain(&p->err_fd, &p->err, &p->err_trunc);
    if (!p->reaped) {
        int st;
        pid_t r = waitpid(p->pid, &st, WNOHANG);

        if (r == p->pid) {
            p->reaped = 1;
            p->status = st;
            /* Final drain: whatever the child wrote before exiting is in the pipes. */
            drain(&p->out_fd, &p->out, &p->out_trunc);
            drain(&p->err_fd, &p->err, &p->err_trunc);
            close_fd(&p->in_fd);
            close_fd(&p->out_fd);
            close_fd(&p->err_fd);
        }
    }
}

static void poll_fill(fd_set *r, fd_set *w, int *maxfd)
{
    int k;

    for (k = 0; k < MAX_PROCS; k++) {
        proc *p = &procs[k];

        if (!p->used || p->reaped)
            continue;
        if (p->out_fd >= 0) {
            FD_SET(p->out_fd, r);
            if (p->out_fd > *maxfd) *maxfd = p->out_fd;
        }
        if (p->err_fd >= 0) {
            FD_SET(p->err_fd, r);
            if (p->err_fd > *maxfd) *maxfd = p->err_fd;
        }
        if (p->in_fd >= 0) {
            FD_SET(p->in_fd, w);
            if (p->in_fd > *maxfd) *maxfd = p->in_fd;
        }
    }
}

static void poll_service(void)
{
    int k;

    for (k = 0; k < MAX_PROCS; k++)
        if (procs[k].used && !procs[k].reaped)
            service_proc(&procs[k]);
}

static const qga_poller poller = { poll_fill, poll_service };

static void set_nonblock_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static void free_proc(proc *p)
{
    close_fd(&p->in_fd);
    close_fd(&p->out_fd);
    close_fd(&p->err_fd);
    sb_free(&p->in);
    sb_free(&p->out);
    sb_free(&p->err);
    p->used = 0;
}

static char **string_array(const json *a)
{
    char **v = (char **)qga_xmalloc((a->n + 1) * sizeof *v);
    size_t k;

    for (k = 0; k < a->n; k++)
        v[k] = a->items[k]->s;
    v[a->n] = NULL;
    return v;
}

static int strings_only(const json *a)
{
    size_t k;

    for (k = 0; k < a->n; k++)
        if (a->items[k]->type != J_STR)
            return 0;
    return 1;
}

static int cmd_exec(const json *args, json **ret, char *err, size_t errlen)
{
    const char *path = NULL, *input = NULL;
    const json *jarg = json_obj_get(args, "arg");
    const json *jenv = json_obj_get(args, "env");
    int capture = 0, k, in_p[2], out_p[2], err_p[2], st_p[2];
    char **argv, **envp = NULL, ebuf[16];
    proc *p;
    pid_t pid;
    ssize_t n;

    if (qga_need_str(args, "path", &path, err, errlen) ||
        qga_opt_str(args, "input-data", &input, err, errlen) ||
        qga_opt_bool(args, "capture-output", &capture, err, errlen))
        return 1;
    if (jarg && (jarg->type != J_ARR || !strings_only(jarg)))
        return qga_seterr(err, errlen, "Invalid parameter type for 'arg', expected: array of string");
    if (jenv && (jenv->type != J_ARR || !strings_only(jenv)))
        return qga_seterr(err, errlen, "Invalid parameter type for 'env', expected: array of string");

    for (k = 0; k < MAX_PROCS && procs[k].used; k++)
        ;
    if (k == MAX_PROCS)
        return qga_seterr(err, errlen, "too many unreaped guest-exec processes");
    p = &procs[k];
    memset(p, 0, sizeof *p);
    p->in_fd = p->out_fd = p->err_fd = -1;
    sb_init(&p->in);
    sb_init(&p->out);
    sb_init(&p->err);
    if (input && base64_decode(input, strlen(input), &p->in) != 0) {
        sb_free(&p->in);
        return qga_seterr(err, errlen, "invalid base64 in 'input-data'");
    }

    /* argv[0] is path; the protocol's "arg" list supplies argv[1..]. A missing /usr/bin/stat
     * (10.0/10.1) is replaced by this binary's own stat applet. */
    {
        size_t na = jarg ? jarg->n : 0, i, front = 1;
        int applet = strcmp(path, "/usr/bin/stat") == 0 && access(path, X_OK) != 0 && qga_self_path[0];

        if (applet) {
            front = 3;
            path = qga_self_path;
        }
        argv = (char **)qga_xmalloc((na + front + 1) * sizeof *argv);
        argv[0] = (char *)path;
        if (applet) {
            argv[1] = "--applet";
            argv[2] = "stat";
        }
        for (i = 0; i < na; i++)
            argv[i + front] = jarg->items[i]->s;
        argv[na + front] = NULL;
    }
    if (jenv)
        envp = string_array(jenv);

    if (pipe(in_p) < 0 || pipe(out_p) < 0 || pipe(err_p) < 0 || pipe(st_p) < 0) {
        free(argv);
        free(envp);
        sb_free(&p->in);
        return qga_seterr(err, errlen, "pipe failed: %s", strerror(errno));
    }
    fcntl(st_p[1], F_SETFD, FD_CLOEXEC);    /* closes on exec: parent sees EOF = success */

    pid = fork();
    if (pid < 0) {
        int e = errno;
        for (k = 0; k < 2; k++) {
            close(in_p[k]); close(out_p[k]); close(err_p[k]); close(st_p[k]);
        }
        free(argv);
        free(envp);
        sb_free(&p->in);
        return qga_seterr(err, errlen, "fork failed: %s", strerror(e));
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);

        signal(SIGPIPE, SIG_DFL);
        dup2(in_p[0], 0);
        dup2(capture ? out_p[1] : devnull, 1);
        dup2(capture ? err_p[1] : devnull, 2);
        for (k = 3; k < 256; k++)
            if (k != st_p[1])
                close(k);
        if (envp)
            environ = envp;
        execvp(path, argv);
        sprintf(ebuf, "%d", errno);
        write(st_p[1], ebuf, strlen(ebuf));
        _exit(127);
    }

    close(in_p[0]);
    close(out_p[1]);
    close(err_p[1]);
    close(st_p[1]);
    free(argv);
    free(envp);

    /* The child reports exec failure through st_p; EOF means exec succeeded. */
    n = read(st_p[0], ebuf, sizeof ebuf - 1);
    close(st_p[0]);
    if (n > 0) {
        ebuf[n] = '\0';
        if (qga_exec_strict) {
            int status;

            waitpid(pid, &status, 0);
            close(in_p[1]);
            close(out_p[0]);
            close(err_p[0]);
            sb_free(&p->in);
            return qga_seterr(err, errlen, "Failed to execute child process \"%s\" (%s)",
                              path, strerror(atoi(ebuf)));
        }
        /* Default, matching the Perl agent the host tooling was written against: the
         * request succeeds, the child exits 127 and its stderr says why. */
        if (capture) {
            char msg[512];

            sprintf(msg, "exec %.300s: %s\n", path, strerror(atoi(ebuf)));
            sb_puts(&p->err, msg);
        }
    }

    p->used = 1;
    p->pid = pid;
    p->capture = capture;
    p->in_fd = in_p[1];
    p->out_fd = out_p[0];
    p->err_fd = err_p[0];
    set_nonblock_cloexec(p->in_fd);
    set_nonblock_cloexec(p->out_fd);
    set_nonblock_cloexec(p->err_fd);
    service_proc(p);                /* start feeding stdin right away */

    {
        json *o = json_obj();
        json_obj_set(o, "pid", json_int(pid));
        *ret = o;
    }
    return 0;
}

static int cmd_exec_status(const json *args, json **ret, char *err, size_t errlen)
{
    qga_i64 pid = 0;
    json *o;
    proc *p = NULL;
    int k;

    if (qga_need_int(args, "pid", &pid, err, errlen))
        return 1;
    for (k = 0; k < MAX_PROCS; k++)
        if (procs[k].used && procs[k].pid == pid)
            p = &procs[k];
    if (!p)
        return qga_seterr(err, errlen, "PID %lld does not exist", pid);

    service_proc(p);
    o = json_obj();
    json_obj_set(o, "exited", json_bool(p->reaped));
    if (p->reaped) {
        if (WIFEXITED(p->status))
            json_obj_set(o, "exitcode", json_int(WEXITSTATUS(p->status)));
        else if (WIFSIGNALED(p->status))
            json_obj_set(o, "signal", json_int(WTERMSIG(p->status)));
        if (p->capture) {
            sbuf enc;

            sb_init(&enc);
            base64_encode((const unsigned char *)(p->out.p ? p->out.p : ""), p->out.len, &enc);
            json_obj_set(o, "out-data", json_strn(sb_cstr(&enc), enc.len));
            sb_reset(&enc);
            base64_encode((const unsigned char *)(p->err.p ? p->err.p : ""), p->err.len, &enc);
            json_obj_set(o, "err-data", json_strn(sb_cstr(&enc), enc.len));
            sb_free(&enc);
            json_obj_set(o, "out-truncated", json_bool(p->out_trunc));
            json_obj_set(o, "err-truncated", json_bool(p->err_trunc));
        }
        free_proc(p);               /* result delivered: like upstream qemu-ga, the pid is forgotten */
    }
    *ret = o;
    return 0;
}

void qga_exec_register(void)
{
    qga_register_ex("guest-exec", cmd_exec, QGA_BLOCK_FROZEN);
    qga_register("guest-exec-status", cmd_exec_status);
    qga_add_poller(&poller);
}
