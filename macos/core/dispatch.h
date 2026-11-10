/* dispatch.h - QGA command registry and request/response handling. */
#ifndef QGA_DISPATCH_H
#define QGA_DISPATCH_H

#include "json.h"

#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#define QGA_VERSION "0.3.0"

/* Handler contract: return 0 and optionally set *ret (NULL means `{}`), or
 * return non-zero after writing a message to err (reported as GenericError).
 * args is the request's "arguments" object, or NULL if absent. */
typedef int (*qga_handler)(const json *args, json **ret, char *err, size_t errlen);

#define QGA_BLOCK_FROZEN 2  /* refused while guest-fsfreeze-freeze is in effect */
#define QGA_NOREPLY 1   /* success-response: false (guest-shutdown): send nothing on success */

void qga_register(const char *name, qga_handler fn);
void qga_register_ex(const char *name, qga_handler fn, int flags);
int qga_command_noreply(size_t i);
size_t qga_ncommands(void);
const char *qga_command_name(size_t i);

/* guest-sync-delimited: prefix the next response with a 0xFF byte. */
void qga_request_delimiter(void);

/* Handles one request line (no trailing newline) and appends the complete
 * response, including the trailing '\n', to out. */
void qga_handle(const char *line, size_t len, sbuf *out);

/* Argument helpers: return 0 on success, else 1 with a QGA-style message in err. */
int qga_need_int(const json *args, const char *key, qga_i64 *out, char *err, size_t errlen);
int qga_need_str(const json *args, const char *key, const char **out, char *err, size_t errlen);
int qga_opt_int(const json *args, const char *key, qga_i64 *out, char *err, size_t errlen);
int qga_opt_str(const json *args, const char *key, const char **out, char *err, size_t errlen);
int qga_opt_bool(const json *args, const char *key, int *out, char *err, size_t errlen);

/* Background work for commands that outlive their request (guest-exec). fill adds
 * descriptors to wait for; service runs after every select() wakeup or timeout. */
typedef struct {
    void (*fill)(fd_set *r, fd_set *w, int *maxfd);
    void (*service)(void);
} qga_poller;

void qga_add_poller(const qga_poller *p);
void qga_poll_fill(fd_set *r, fd_set *w, int *maxfd);
void qga_poll_service(void);

extern int qga_frozen;              /* set by commands/freeze.c */

void qga_protocol_register(void);   /* commands/protocol.c */
void qga_file_register(void);       /* commands/file.c */
void qga_exec_register(void);       /* commands/exec.c */
extern int qga_exec_strict;         /* exec failure fails guest-exec instead of exit code 127 */
void qga_system_register(void);     /* commands/system.c */
void qga_hw_register(void);         /* commands/hw.c: memory, cpustats, disks */
int  qga_hw_have_iokit(void);       /* 0 in the x86_64 slice on Tiger: no diskstats there */
void qga_freeze_register(void);     /* commands/freeze.c */
void qga_ssh_register(void);        /* commands/ssh.c */
void qga_user_register(void);       /* commands/user.c */

/* Applets: the agent re-executes itself to stand in for tools missing on old releases. */
extern char qga_self_path[1024];    /* absolute path of this binary, set by main() */
int qga_applet_stat(int argc, char **argv);

#endif
