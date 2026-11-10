#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 32
#define QGA_I64_MAX 9223372036854775807LL

static json *json_new(jtype t)
{
    json *j = (json *)qga_xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->type = t;
    return j;
}

json *json_null(void) { return json_new(J_NULL); }

json *json_bool(int b)
{
    json *j = json_new(J_BOOL);
    j->b = b != 0;
    return j;
}

json *json_int(qga_i64 i)
{
    json *j = json_new(J_INT);
    j->i = i;
    return j;
}

json *json_real(double d)
{
    json *j = json_new(J_REAL);
    j->d = d;
    return j;
}

json *json_strn(const char *s, size_t n)
{
    json *j = json_new(J_STR);
    j->s = qga_xstrndup(s, n);
    return j;
}

json *json_str(const char *s) { return json_strn(s, strlen(s)); }
json *json_arr(void) { return json_new(J_ARR); }
json *json_obj(void) { return json_new(J_OBJ); }

void json_free(json *j)
{
    size_t k;

    if (!j)
        return;
    free(j->s);
    for (k = 0; k < j->n; k++) {
        json_free(j->items[k]);
        if (j->keys)
            free(j->keys[k]);
    }
    free(j->items);
    free(j->keys);
    free(j);
}

static void grow(json *j)
{
    if (j->n < j->cap)
        return;
    j->cap = j->cap ? j->cap * 2 : 4;
    j->items = (json **)qga_xrealloc(j->items, j->cap * sizeof *j->items);
    if (j->type == J_OBJ)
        j->keys = (char **)qga_xrealloc(j->keys, j->cap * sizeof *j->keys);
}

void json_arr_add(json *arr, json *v)
{
    grow(arr);
    arr->items[arr->n++] = v;
}

void json_obj_set(json *obj, const char *key, json *v)
{
    size_t k;

    for (k = 0; k < obj->n; k++) {
        if (strcmp(obj->keys[k], key) == 0) {
            json_free(obj->items[k]);
            obj->items[k] = v;
            return;
        }
    }
    grow(obj);
    obj->keys[obj->n] = qga_xstrdup(key);
    obj->items[obj->n] = v;
    obj->n++;
}

json *json_obj_get(const json *obj, const char *key)
{
    size_t k;

    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (k = 0; k < obj->n; k++)
        if (strcmp(obj->keys[k], key) == 0)
            return obj->items[k];
    return NULL;
}

/* ---- serializer -------------------------------------------------------- */

static void put_string(const char *s, sbuf *out)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)s;

    sb_putc(out, '"');
    for (; *p; p++) {
        switch (*p) {
        case '"':  sb_puts(out, "\\\""); break;
        case '\\': sb_puts(out, "\\\\"); break;
        case '\n': sb_puts(out, "\\n"); break;
        case '\r': sb_puts(out, "\\r"); break;
        case '\t': sb_puts(out, "\\t"); break;
        case '\b': sb_puts(out, "\\b"); break;
        case '\f': sb_puts(out, "\\f"); break;
        default:
            if (*p < 0x20) {
                sb_puts(out, "\\u00");
                sb_putc(out, hex[*p >> 4]);
                sb_putc(out, hex[*p & 15]);
            } else {
                sb_putc(out, *p);   /* UTF-8 bytes pass through */
            }
        }
    }
    sb_putc(out, '"');
}

void json_serialize(const json *j, sbuf *out)
{
    char tmp[40];
    size_t k;

    switch (j->type) {
    case J_NULL: sb_puts(out, "null"); break;
    case J_BOOL: sb_puts(out, j->b ? "true" : "false"); break;
    case J_INT:
        sprintf(tmp, "%lld", j->i);
        sb_puts(out, tmp);
        break;
    case J_REAL:
        if (j->d != j->d || j->d - j->d != 0) {     /* NaN or infinity */
            sb_puts(out, "null");
        } else {
            sprintf(tmp, "%.15g", j->d);
            sb_puts(out, tmp);
        }
        break;
    case J_STR: put_string(j->s, out); break;
    case J_ARR:
        sb_putc(out, '[');
        for (k = 0; k < j->n; k++) {
            if (k)
                sb_putc(out, ',');
            json_serialize(j->items[k], out);
        }
        sb_putc(out, ']');
        break;
    case J_OBJ:
        sb_putc(out, '{');
        for (k = 0; k < j->n; k++) {
            if (k)
                sb_putc(out, ',');
            put_string(j->keys[k], out);
            sb_putc(out, ':');
            json_serialize(j->items[k], out);
        }
        sb_putc(out, '}');
        break;
    }
}

/* ---- parser ------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    const char *err;
} pstate;

static json *parse_value(pstate *ps, int depth);

static void skip_ws(pstate *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int fail(pstate *ps, const char *msg)
{
    if (!ps->err)
        ps->err = msg;
    return 0;
}

static int hex4(pstate *ps, unsigned long *out)
{
    unsigned long v = 0;
    int k;

    if (ps->end - ps->p < 4)
        return fail(ps, "truncated \\u escape");
    for (k = 0; k < 4; k++) {
        char c = *ps->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned long)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned long)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned long)(c - 'A' + 10);
        else return fail(ps, "bad \\u escape");
    }
    *out = v;
    return 1;
}

static void put_utf8(sbuf *b, unsigned long cp)
{
    if (cp < 0x80) {
        sb_putc(b, (int)cp);
    } else if (cp < 0x800) {
        sb_putc(b, (int)(0xC0 | (cp >> 6)));
        sb_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(b, (int)(0xE0 | (cp >> 12)));
        sb_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (int)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(b, (int)(0xF0 | (cp >> 18)));
        sb_putc(b, (int)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(b, (int)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(b, (int)(0x80 | (cp & 0x3F)));
    }
}

/* Parses a string literal at ps->p (which points at the opening quote). */
static int parse_string(pstate *ps, sbuf *b)
{
    ps->p++;
    for (;;) {
        unsigned char c;

        if (ps->p >= ps->end)
            return fail(ps, "unterminated string");
        c = (unsigned char)*ps->p++;
        if (c == '"')
            return 1;
        if (c < 0x20)
            return fail(ps, "control character in string");
        if (c != '\\') {
            sb_putc(b, c);
            continue;
        }
        if (ps->p >= ps->end)
            return fail(ps, "unterminated string");
        switch (*ps->p++) {
        case '"':  sb_putc(b, '"'); break;
        case '\\': sb_putc(b, '\\'); break;
        case '/':  sb_putc(b, '/'); break;
        case 'b':  sb_putc(b, '\b'); break;
        case 'f':  sb_putc(b, '\f'); break;
        case 'n':  sb_putc(b, '\n'); break;
        case 'r':  sb_putc(b, '\r'); break;
        case 't':  sb_putc(b, '\t'); break;
        case 'u': {
            unsigned long cp, lo;

            if (!hex4(ps, &cp))
                return 0;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u')
                    return fail(ps, "lone surrogate");
                ps->p += 2;
                if (!hex4(ps, &lo))
                    return 0;
                if (lo < 0xDC00 || lo > 0xDFFF)
                    return fail(ps, "bad surrogate pair");
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return fail(ps, "lone surrogate");
            }
            if (cp == 0)
                return fail(ps, "NUL in string");
            put_utf8(b, cp);
            break;
        }
        default:
            return fail(ps, "bad escape");
        }
    }
}

static json *parse_number(pstate *ps)
{
    const char *start = ps->p;
    const char *q = ps->p;
    int is_real = 0;
    int digits;
    char tmp[64];
    size_t n;

    if (q < ps->end && *q == '-')
        q++;
    digits = 0;
    if (q < ps->end && *q == '0') {
        q++;
        digits = 1;
    } else {
        while (q < ps->end && *q >= '0' && *q <= '9') {
            q++;
            digits++;
        }
    }
    if (!digits) {
        fail(ps, "bad number");
        return NULL;
    }
    if (q < ps->end && *q == '.') {
        int frac = 0;
        is_real = 1;
        q++;
        while (q < ps->end && *q >= '0' && *q <= '9') {
            q++;
            frac++;
        }
        if (!frac) {
            fail(ps, "bad number");
            return NULL;
        }
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        int exp = 0;
        is_real = 1;
        q++;
        if (q < ps->end && (*q == '+' || *q == '-'))
            q++;
        while (q < ps->end && *q >= '0' && *q <= '9') {
            q++;
            exp++;
        }
        if (!exp) {
            fail(ps, "bad number");
            return NULL;
        }
    }
    n = (size_t)(q - start);
    if (n >= sizeof tmp) {
        fail(ps, "number too long");
        return NULL;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    ps->p = q;

    if (!is_real && digits <= 19) {
        const char *d = tmp + (tmp[0] == '-');
        qga_i64 v = 0;
        int ok = 1;

        for (; *d; d++) {
            int dig = *d - '0';
            if (v > (QGA_I64_MAX - dig) / 10) {
                ok = 0;
                break;
            }
            v = v * 10 + dig;
        }
        if (ok)
            return json_int(tmp[0] == '-' ? -v : v);
    }
    return json_real(strtod(tmp, NULL));
}

static int expect_word(pstate *ps, const char *w)
{
    size_t n = strlen(w);

    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, w, n) != 0)
        return fail(ps, "invalid literal");
    ps->p += n;
    return 1;
}

static json *parse_array(pstate *ps, int depth)
{
    json *arr = json_arr();

    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        json *v = parse_value(ps, depth + 1);

        if (!v) {
            json_free(arr);
            return NULL;
        }
        json_arr_add(arr, v);
        skip_ws(ps);
        if (ps->p >= ps->end) {
            fail(ps, "unterminated array");
            json_free(arr);
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ']') {
            ps->p++;
            return arr;
        }
        fail(ps, "expected ',' or ']'");
        json_free(arr);
        return NULL;
    }
}

static json *parse_object(pstate *ps, int depth)
{
    json *obj = json_obj();

    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        sbuf key;
        json *v;

        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            fail(ps, "expected object key");
            json_free(obj);
            return NULL;
        }
        sb_init(&key);
        if (!parse_string(ps, &key)) {
            sb_free(&key);
            json_free(obj);
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            fail(ps, "expected ':'");
            sb_free(&key);
            json_free(obj);
            return NULL;
        }
        ps->p++;
        v = parse_value(ps, depth + 1);
        if (!v) {
            sb_free(&key);
            json_free(obj);
            return NULL;
        }
        json_obj_set(obj, sb_cstr(&key), v);
        sb_free(&key);
        skip_ws(ps);
        if (ps->p >= ps->end) {
            fail(ps, "unterminated object");
            json_free(obj);
            return NULL;
        }
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == '}') {
            ps->p++;
            return obj;
        }
        fail(ps, "expected ',' or '}'");
        json_free(obj);
        return NULL;
    }
}

static json *parse_value(pstate *ps, int depth)
{
    skip_ws(ps);
    if (depth > JSON_MAX_DEPTH) {
        fail(ps, "nesting too deep");
        return NULL;
    }
    if (ps->p >= ps->end) {
        fail(ps, "unexpected end of input");
        return NULL;
    }
    switch (*ps->p) {
    case '{': return parse_object(ps, depth);
    case '[': return parse_array(ps, depth);
    case '"': {
        sbuf b;
        json *j;

        sb_init(&b);
        if (!parse_string(ps, &b)) {
            sb_free(&b);
            return NULL;
        }
        j = json_strn(sb_cstr(&b), b.len);
        sb_free(&b);
        return j;
    }
    case 't': return expect_word(ps, "true") ? json_bool(1) : NULL;
    case 'f': return expect_word(ps, "false") ? json_bool(0) : NULL;
    case 'n': return expect_word(ps, "null") ? json_null() : NULL;
    default:
        if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
            return parse_number(ps);
        fail(ps, "unexpected character");
        return NULL;
    }
}

json *json_parse(const char *s, size_t len, const char **err)
{
    pstate ps;
    json *j;

    ps.p = s;
    ps.end = s + len;
    ps.err = NULL;
    j = parse_value(&ps, 0);
    if (j) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            json_free(j);
            j = NULL;
            ps.err = "trailing data after value";
        }
    }
    if (!j && err)
        *err = ps.err ? ps.err : "parse error";
    return j;
}
