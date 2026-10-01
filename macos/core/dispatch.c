#include "dispatch.h"

#include <string.h>

#define MAX_COMMANDS 128

typedef struct {
    const char *name;
    qga_handler fn;
    int flags;
} cmd;

static cmd table[MAX_COMMANDS];
static size_t ncmds = 0;
static int delimit_next = 0;

void qga_register_ex(const char *name, qga_handler fn, int flags)
{
    if (ncmds < MAX_COMMANDS) {
        table[ncmds].name = name;
        table[ncmds].fn = fn;
        table[ncmds].flags = flags;
        ncmds++;
    }
}

void qga_register(const char *name, qga_handler fn)
{
    qga_register_ex(name, fn, 0);
}

int qga_command_noreply(size_t i)
{
    return i < ncmds && (table[i].flags & QGA_NOREPLY);
}

size_t qga_ncommands(void) { return ncmds; }

const char *qga_command_name(size_t i)
{
    return i < ncmds ? table[i].name : NULL;
}

void qga_request_delimiter(void) { delimit_next = 1; }

static json *error_obj(const char *klass, const char *desc)
{
    json *e = json_obj();
    json *o = json_obj();

    json_obj_set(e, "class", json_str(klass));
    json_obj_set(e, "desc", json_str(desc));
    json_obj_set(o, "error", e);
    return o;
}

static void emit(json *resp, sbuf *out)
{
    if (delimit_next) {
        sb_putc(out, 0xFF);
        delimit_next = 0;
    }
    json_serialize(resp, out);
    sb_putc(out, '\n');
    json_free(resp);
}

void qga_handle(const char *line, size_t len, sbuf *out)
{
    const char *perr = NULL;
    json *req, *exec, *args, *id, *resp = NULL;
    char err[160];
    size_t k;

    req = json_parse(line, len, &perr);
    if (!req) {
        char msg[128];
        strcpy(msg, "JSON parse error: ");
        strncat(msg, perr, sizeof msg - strlen(msg) - 1);
        emit(error_obj("GenericError", msg), out);
        return;
    }
    exec = json_obj_get(req, "execute");
    if (req->type != J_OBJ || !exec || exec->type != J_STR) {
        json_free(req);
        emit(error_obj("GenericError", "Dictionary missing 'execute' key"), out);
        return;
    }
    args = json_obj_get(req, "arguments");
    if (args && args->type == J_NULL)
        args = NULL;
    if (args && args->type != J_OBJ) {
        json_free(req);
        emit(error_obj("GenericError", "'arguments' must be an object"), out);
        return;
    }

    for (k = 0; k < ncmds; k++)
        if (strcmp(table[k].name, exec->s) == 0)
            break;

    if (k == ncmds) {
        char msg[160];
        strcpy(msg, "The command ");
        strncat(msg, exec->s, sizeof msg - strlen(msg) - 32);
        strcat(msg, " has not been found");
        resp = error_obj("CommandNotFound", msg);
    } else {
        json *ret = NULL;

        err[0] = '\0';
        if (table[k].fn(args, &ret, err, sizeof err) == 0) {
            if (table[k].flags & QGA_NOREPLY) {
                json_free(ret);
                json_free(req);
                return;
            }
            resp = json_obj();
            json_obj_set(resp, "return", ret ? ret : json_obj());
        } else {
            json_free(ret);
            resp = error_obj("GenericError", err[0] ? err : "command failed");
        }
    }

    id = json_obj_get(req, "id");
    if (id) {
        /* Echo the id: move it out of the request into the response. */
        json *cp = (json *)qga_xmalloc(sizeof *cp);

        *cp = *id;
        memset(id, 0, sizeof *id);      /* json_free(req) must not free it */
        id->type = J_NULL;
        json_obj_set(resp, "id", cp);
    }
    json_free(req);
    emit(resp, out);
}

/* ---- argument helpers ---------------------------------------------------- */

static const json *find_arg(const json *args, const char *key)
{
    return args ? json_obj_get(args, key) : NULL;
}

static int bad_type(const char *key, const char *want, char *err, size_t errlen)
{
    return qga_seterr(err, errlen, "Invalid parameter type for '%s', expected: %s", key, want);
}

int qga_opt_int(const json *args, const char *key, qga_i64 *out, char *err, size_t errlen)
{
    const json *v = find_arg(args, key);

    if (!v)
        return 0;
    if (v->type != J_INT)
        return bad_type(key, "integer", err, errlen);
    *out = v->i;
    return 0;
}

int qga_need_int(const json *args, const char *key, qga_i64 *out, char *err, size_t errlen)
{
    if (!find_arg(args, key))
        return qga_seterr(err, errlen, "Parameter '%s' is missing", key);
    return qga_opt_int(args, key, out, err, errlen);
}

int qga_opt_str(const json *args, const char *key, const char **out, char *err, size_t errlen)
{
    const json *v = find_arg(args, key);

    if (!v)
        return 0;
    if (v->type != J_STR)
        return bad_type(key, "string", err, errlen);
    *out = v->s;
    return 0;
}

int qga_need_str(const json *args, const char *key, const char **out, char *err, size_t errlen)
{
    if (!find_arg(args, key))
        return qga_seterr(err, errlen, "Parameter '%s' is missing", key);
    return qga_opt_str(args, key, out, err, errlen);
}

int qga_opt_bool(const json *args, const char *key, int *out, char *err, size_t errlen)
{
    const json *v = find_arg(args, key);

    if (!v)
        return 0;
    if (v->type != J_BOOL)
        return bad_type(key, "boolean", err, errlen);
    *out = v->b;
    return 0;
}

/* ---- pollers ------------------------------------------------------------- */

#define MAX_POLLERS 8
static const qga_poller *pollers[MAX_POLLERS];
static size_t npollers = 0;

void qga_add_poller(const qga_poller *p)
{
    if (npollers < MAX_POLLERS)
        pollers[npollers++] = p;
}

void qga_poll_fill(fd_set *r, fd_set *w, int *maxfd)
{
    size_t k;

    for (k = 0; k < npollers; k++)
        pollers[k]->fill(r, w, maxfd);
}

void qga_poll_service(void)
{
    size_t k;

    for (k = 0; k < npollers; k++)
        pollers[k]->service();
}
