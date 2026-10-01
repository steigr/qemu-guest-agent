#include "selftest.h"

#include "core/base64.h"
#include "core/dispatch.h"
#include "core/json.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void roundtrip(const char *in, const char *expect)
{
    const char *err;
    json *j = json_parse(in, strlen(in), &err);
    sbuf b;

    CHECK(j != NULL);
    if (!j)
        return;
    sb_init(&b);
    json_serialize(j, &b);
    CHECK(strcmp(sb_cstr(&b), expect) == 0);
    if (strcmp(sb_cstr(&b), expect) != 0)
        printf("  got %s, want %s\n", sb_cstr(&b), expect);
    sb_free(&b);
    json_free(j);
}

static void bad(const char *in)
{
    const char *err = NULL;
    json *j = json_parse(in, strlen(in), &err);

    CHECK(j == NULL && err != NULL);
    if (j)
        printf("  accepted: %s\n", in);
    json_free(j);
}

static void test_json(void)
{
    char deep[200];
    int k;
    json *j;
    const char *err;

    roundtrip("{\"a\":[1,-2,true,false,null],\"b\":\"x\"}",
              "{\"a\":[1,-2,true,false,null],\"b\":\"x\"}");
    roundtrip("  [ ]  ", "[]");
    roundtrip("\"a\\n\\t\\\"\\\\\\/\\u0001\"", "\"a\\n\\t\\\"\\\\/\\u0001\"");
    roundtrip("\"\\u00e4\\ud83d\\ude00\"", "\"\xc3\xa4\xf0\x9f\x98\x80\"");
    roundtrip("9223372036854775807", "9223372036854775807");
    roundtrip("-9223372036854775807", "-9223372036854775807");
    roundtrip("0.5", "0.5");
    roundtrip("{\"k\":1,\"k\":2}", "{\"k\":2}");

    bad("");
    bad("{");
    bad("[1,]");
    bad("{\"a\" 1}");
    bad("{'a':1}");
    bad("01x");
    bad("1.");
    bad("\"\\ud800\"");
    bad("\"\\u0000\"");
    bad("\"unterminated");
    bad("{} {}");
    bad("tru");

    for (k = 0; k < 100; k++)
        deep[k] = '[';
    deep[100] = '\0';
    bad(deep);

    j = json_parse("18446744073709551615", 20, &err);   /* overflows int64 */
    CHECK(j && j->type == J_REAL);
    json_free(j);
}

static void b64(const char *plain, const char *enc)
{
    sbuf e, d;

    sb_init(&e);
    sb_init(&d);
    base64_encode((const unsigned char *)plain, strlen(plain), &e);
    CHECK(strcmp(sb_cstr(&e), enc) == 0);
    CHECK(base64_decode(enc, strlen(enc), &d) == 0);
    CHECK(d.len == strlen(plain) && memcmp(d.p ? d.p : "", plain, d.len) == 0);
    sb_free(&e);
    sb_free(&d);
}

static void test_base64(void)
{
    sbuf d;
    unsigned char all[256];
    sbuf e;
    int k;

    b64("", "");
    b64("f", "Zg==");
    b64("fo", "Zm8=");
    b64("foo", "Zm9v");
    b64("foob", "Zm9vYg==");
    b64("fooba", "Zm9vYmE=");
    b64("foobar", "Zm9vYmFy");

    sb_init(&d);
    CHECK(base64_decode("Zg=", 3, &d) == -1);
    sb_reset(&d);
    CHECK(base64_decode("Zg=x", 4, &d) == -1);
    sb_reset(&d);
    CHECK(base64_decode("Z*==", 4, &d) == -1);
    sb_reset(&d);
    CHECK(base64_decode("Zh==", 4, &d) == -1);      /* non-zero pad bits */
    sb_free(&d);

    for (k = 0; k < 256; k++)
        all[k] = (unsigned char)k;
    sb_init(&e);
    sb_init(&d);
    base64_encode(all, sizeof all, &e);
    CHECK(base64_decode(e.p, e.len, &d) == 0);
    CHECK(d.len == 256 && memcmp(d.p, all, 256) == 0);
    sb_free(&e);
    sb_free(&d);
}

static int reply_is(const char *req, const char *expect)
{
    sbuf out;
    int ok;

    sb_init(&out);
    qga_handle(req, strlen(req), &out);
    ok = strcmp(sb_cstr(&out), expect) == 0;
    if (!ok)
        printf("  req %s\n  got %s  want %s", req, sb_cstr(&out), expect);
    sb_free(&out);
    return ok;
}

static void test_dispatch(void)
{
    sbuf out;
    const char *err;
    json *j, *cmds;

    qga_protocol_register();

    CHECK(reply_is("{\"execute\":\"guest-ping\"}", "{\"return\":{}}\n"));
    CHECK(reply_is("{\"execute\":\"guest-ping\",\"id\":\"abc\"}",
                   "{\"return\":{},\"id\":\"abc\"}\n"));
    CHECK(reply_is("{\"execute\":\"guest-sync\",\"arguments\":{\"id\":123456789012}}",
                   "{\"return\":123456789012}\n"));
    CHECK(reply_is("{\"execute\":\"guest-sync\"}",
                   "{\"error\":{\"class\":\"GenericError\","
                   "\"desc\":\"Parameter 'id' is missing\"}}\n"));
    CHECK(reply_is("{\"execute\":\"guest-sync\",\"arguments\":{\"id\":\"x\"}}",
                   "{\"error\":{\"class\":\"GenericError\","
                   "\"desc\":\"Invalid parameter type for 'id', expected: integer\"}}\n"));
    CHECK(reply_is("{\"execute\":\"nope\"}",
                   "{\"error\":{\"class\":\"CommandNotFound\","
                   "\"desc\":\"The command nope has not been found\"}}\n"));
    CHECK(reply_is("{\"foo\":1}",
                   "{\"error\":{\"class\":\"GenericError\","
                   "\"desc\":\"Dictionary missing 'execute' key\"}}\n"));
    CHECK(reply_is("[1]",
                   "{\"error\":{\"class\":\"GenericError\","
                   "\"desc\":\"Dictionary missing 'execute' key\"}}\n"));
    CHECK(reply_is("{\"execute\":\"guest-ping\",\"arguments\":3}",
                   "{\"error\":{\"class\":\"GenericError\","
                   "\"desc\":\"'arguments' must be an object\"}}\n"));

    /* guest-sync-delimited: 0xFF precedes exactly this one response. */
    CHECK(reply_is("{\"execute\":\"guest-sync-delimited\",\"arguments\":{\"id\":7}}",
                   "\xff{\"return\":7}\n"));
    CHECK(reply_is("{\"execute\":\"guest-ping\"}", "{\"return\":{}}\n"));

    /* guest-info advertises every registered command. */
    sb_init(&out);
    qga_handle("{\"execute\":\"guest-info\"}", 24, &out);
    j = json_parse(out.p, out.len, &err);
    CHECK(j != NULL);
    if (j) {
        json *ret = json_obj_get(j, "return");
        cmds = json_obj_get(ret, "supported_commands");
        CHECK(cmds && cmds->type == J_ARR && cmds->n == qga_ncommands());
        CHECK(json_obj_get(ret, "version") != NULL);
        json_free(j);
    }
    sb_free(&out);
}

/* Runs one request and returns the parsed reply's "return" member (caller frees the
 * whole reply via *keep), or NULL after printing the reply if it was an error. */
static json *call(const char *req, json **keep)
{
    sbuf out;
    const char *err;
    json *j, *r;

    sb_init(&out);
    qga_handle(req, strlen(req), &out);
    j = json_parse(out.p, out.len, &err);
    sb_free(&out);
    *keep = j;
    r = j ? json_obj_get(j, "return") : NULL;
    if (!r)
        printf("  request %s failed\n", req);
    return r;
}

static void test_file(void)
{
    const char *path = "/tmp/qemu-guest-agent-selftest.bin";
    char req[512];
    json *keep, *r;
    qga_i64 h;
    unsigned char all[256];
    sbuf enc;
    int k;

    for (k = 0; k < 256; k++)
        all[k] = (unsigned char)k;
    sb_init(&enc);
    base64_encode(all, sizeof all, &enc);

    sprintf(req, "{\"execute\":\"guest-file-open\",\"arguments\":{\"path\":\"%s\",\"mode\":\"w+\"}}", path);
    r = call(req, &keep);
    CHECK(r && r->type == J_INT);
    if (!r) {
        json_free(keep);
        sb_free(&enc);
        return;
    }
    h = r->i;
    json_free(keep);

    sprintf(req, "{\"execute\":\"guest-file-write\",\"arguments\":{\"handle\":%lld,\"buf-b64\":\"%s\"}}",
            h, sb_cstr(&enc));
    r = call(req, &keep);
    CHECK(r && json_obj_get(r, "count") && json_obj_get(r, "count")->i == 256);
    json_free(keep);

    sprintf(req, "{\"execute\":\"guest-file-seek\",\"arguments\":{\"handle\":%lld,\"offset\":10,\"whence\":0}}", h);
    r = call(req, &keep);
    CHECK(r && json_obj_get(r, "position") && json_obj_get(r, "position")->i == 10);
    json_free(keep);

    sprintf(req, "{\"execute\":\"guest-file-read\",\"arguments\":{\"handle\":%lld,\"count\":1000}}", h);
    r = call(req, &keep);
    CHECK(r && json_obj_get(r, "count")->i == 246 && json_obj_get(r, "eof")->b == 1);
    {
        sbuf dec;
        json *b = r ? json_obj_get(r, "buf-b64") : NULL;

        sb_init(&dec);
        CHECK(b && base64_decode(b->s, strlen(b->s), &dec) == 0 &&
              dec.len == 246 && memcmp(dec.p, all + 10, 246) == 0);
        sb_free(&dec);
    }
    json_free(keep);

    sprintf(req, "{\"execute\":\"guest-file-flush\",\"arguments\":{\"handle\":%lld}}", h);
    r = call(req, &keep);
    CHECK(r != NULL);
    json_free(keep);
    sprintf(req, "{\"execute\":\"guest-file-close\",\"arguments\":{\"handle\":%lld}}", h);
    r = call(req, &keep);
    CHECK(r != NULL);
    json_free(keep);

    CHECK(reply_is("{\"execute\":\"guest-file-read\",\"arguments\":{\"handle\":999}}",
                   "{\"error\":{\"class\":\"GenericError\",\"desc\":\"handle \\\"999\\\" has not been found\"}}\n"));
    unlink(path);
    sb_free(&enc);
}

static void test_exec(void)
{
    json *keep, *r;
    qga_i64 pid;
    int k, exited = 0;
    sbuf dec;
    json *o;

    /* stdin is passed through, stdout and stderr captured separately, exit code reported. */
    r = call("{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\","
             "\"arg\":[\"-c\",\"cat; echo err >&2; exit 3\"],\"input-data\":\"aGVsbG8=\","
             "\"capture-output\":true}}", &keep);
    CHECK(r && json_obj_get(r, "pid"));
    if (!r) {
        json_free(keep);
        return;
    }
    pid = json_obj_get(r, "pid")->i;
    json_free(keep);

    for (k = 0; k < 100 && !exited; k++) {
        char req[128];

        qga_poll_service();
        sprintf(req, "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%lld}}", pid);
        r = call(req, &keep);
        if (r && json_obj_get(r, "exited")->b) {
            exited = 1;
            o = r;
            CHECK(json_obj_get(o, "exitcode") && json_obj_get(o, "exitcode")->i == 3);
            sb_init(&dec);
            CHECK(base64_decode(json_obj_get(o, "out-data")->s, strlen(json_obj_get(o, "out-data")->s), &dec) == 0
                  && dec.len == 5 && memcmp(dec.p, "hello", 5) == 0);
            sb_reset(&dec);
            CHECK(base64_decode(json_obj_get(o, "err-data")->s, strlen(json_obj_get(o, "err-data")->s), &dec) == 0
                  && dec.len == 4 && memcmp(dec.p, "err\n", 4) == 0);
            sb_free(&dec);
        }
        json_free(keep);
        if (!exited)
            usleep(20000);
    }
    CHECK(exited);

    /* Missing binary: strict mode fails the request... */
    qga_exec_strict = 1;
    r = call("{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/nonexistent/x\"}}", &keep);
    CHECK(r == NULL);
    json_free(keep);
    qga_exec_strict = 0;

    /* ...the default reports exit 127 with the reason on stderr. */
    r = call("{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/nonexistent/x\",\"capture-output\":true}}", &keep);
    CHECK(r && json_obj_get(r, "pid"));
    if (r) {
        char req[128];

        pid = json_obj_get(r, "pid")->i;
        json_free(keep);
        exited = 0;
        for (k = 0; k < 100 && !exited; k++) {
            sprintf(req, "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%lld}}", pid);
            r = call(req, &keep);
            if (r && json_obj_get(r, "exited")->b) {
                exited = 1;
                CHECK(json_obj_get(r, "exitcode") && json_obj_get(r, "exitcode")->i == 127);
                CHECK(strlen(json_obj_get(r, "err-data")->s) > 0);
            }
            json_free(keep);
            if (!exited)
                usleep(20000);
        }
        CHECK(exited);
    }

    /* Death by signal. */
    r = call("{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"kill -9 $$\"]}}", &keep);
    CHECK(r != NULL);
    if (r) {
        char req[128];

        pid = json_obj_get(r, "pid")->i;
        json_free(keep);
        exited = 0;
        for (k = 0; k < 100 && !exited; k++) {
            sprintf(req, "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%lld}}", pid);
            r = call(req, &keep);
            if (r && json_obj_get(r, "exited")->b) {
                exited = 1;
                CHECK(json_obj_get(r, "signal") && json_obj_get(r, "signal")->i == 9);
            }
            json_free(keep);
            if (!exited)
                usleep(20000);
        }
        CHECK(exited);
    }
}

static void test_system(void)
{
    json *keep, *r;

    r = call("{\"execute\":\"guest-get-host-name\"}", &keep);
    CHECK(r && json_obj_get(r, "host-name") && json_obj_get(r, "host-name")->s[0]);
    json_free(keep);
    r = call("{\"execute\":\"guest-get-time\"}", &keep);
    CHECK(r && r->type == J_INT && r->i > 1000000000LL * 1000000000LL);
    json_free(keep);
    r = call("{\"execute\":\"guest-get-load\"}", &keep);
    CHECK(r && json_obj_get(r, "load1m"));
    json_free(keep);
    r = call("{\"execute\":\"guest-get-vcpus\"}", &keep);
    CHECK(r && r->type == J_ARR && r->n >= 1);
    json_free(keep);
    r = call("{\"execute\":\"guest-get-fsinfo\"}", &keep);
    CHECK(r && r->type == J_ARR && r->n >= 1);
    json_free(keep);
    r = call("{\"execute\":\"guest-get-osinfo\"}", &keep);
    CHECK(r && json_obj_get(r, "kernel-release"));
    json_free(keep);
    r = call("{\"execute\":\"guest-get-timezone\"}", &keep);
    CHECK(r && json_obj_get(r, "offset"));
    json_free(keep);
    r = call("{\"execute\":\"guest-network-get-interfaces\"}", &keep);
    CHECK(r && r->type == J_ARR && r->n >= 1);
    json_free(keep);
    r = call("{\"execute\":\"guest-get-users\"}", &keep);
    CHECK(r && r->type == J_ARR);
    json_free(keep);
}

int qga_selftest(void)
{
    test_json();
    test_base64();
    test_dispatch();
    qga_file_register();
    qga_exec_register();
    qga_system_register();
    test_file();
    test_exec();
    test_system();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
