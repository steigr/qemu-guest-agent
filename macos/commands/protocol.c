/* protocol.c - guest-ping, guest-sync*, guest-info. Needs nothing but libc. */
#include "../core/dispatch.h"

#include <stdio.h>
#include <string.h>

static int cmd_ping(const json *args, json **ret, char *err, size_t errlen)
{
    (void)args; (void)ret; (void)err; (void)errlen;
    return 0;
}

static int sync_common(const json *args, json **ret, char *err, size_t errlen)
{
    const json *id = json_obj_get(args, "id");

    if (!id) {
        strncpy(err, "Parameter 'id' is missing", errlen - 1);
        err[errlen - 1] = '\0';
        return 1;
    }
    if (id->type != J_INT) {
        strncpy(err, "Invalid parameter type for 'id', expected: integer", errlen - 1);
        err[errlen - 1] = '\0';
        return 1;
    }
    *ret = json_int(id->i);
    return 0;
}

static int cmd_sync(const json *args, json **ret, char *err, size_t errlen)
{
    return sync_common(args, ret, err, errlen);
}

static int cmd_sync_delimited(const json *args, json **ret, char *err, size_t errlen)
{
    int rc = sync_common(args, ret, err, errlen);

    if (rc == 0)
        qga_request_delimiter();
    return rc;
}

static int cmd_info(const json *args, json **ret, char *err, size_t errlen)
{
    json *o = json_obj();
    json *list = json_arr();
    size_t k;

    (void)args; (void)err; (void)errlen;
    for (k = 0; k < qga_ncommands(); k++) {
        json *c = json_obj();

        json_obj_set(c, "enabled", json_bool(1));
        json_obj_set(c, "name", json_str(qga_command_name(k)));
        json_obj_set(c, "success-response", json_bool(!qga_command_noreply(k)));
        json_arr_add(list, c);
    }
    json_obj_set(o, "version", json_str(QGA_VERSION));
    json_obj_set(o, "supported_commands", list);
    *ret = o;
    return 0;
}

void qga_protocol_register(void)
{
    qga_register("guest-ping", cmd_ping);
    qga_register("guest-sync", cmd_sync);
    qga_register("guest-sync-id", cmd_sync);
    qga_register("guest-sync-delimited", cmd_sync_delimited);
    qga_register("guest-info", cmd_info);
}
