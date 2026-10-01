/* json.h - minimal JSON value tree, parser and serializer. C89, no deps. */
#ifndef QGA_JSON_H
#define QGA_JSON_H

#include "util.h"

typedef enum { J_NULL, J_BOOL, J_INT, J_REAL, J_STR, J_ARR, J_OBJ } jtype;

typedef struct json json;
struct json {
    jtype type;
    int b;              /* J_BOOL */
    qga_i64 i;          /* J_INT */
    double d;           /* J_REAL */
    char *s;            /* J_STR */
    json **items;       /* J_ARR elements, J_OBJ values */
    char **keys;        /* J_OBJ keys (parallel to items) */
    size_t n;
    size_t cap;
};

json *json_null(void);
json *json_bool(int b);
json *json_int(qga_i64 i);
json *json_real(double d);
json *json_str(const char *s);
json *json_strn(const char *s, size_t n);
json *json_arr(void);
json *json_obj(void);
void json_free(json *j);

void json_arr_add(json *arr, json *v);              /* takes ownership of v */
void json_obj_set(json *obj, const char *key, json *v); /* takes ownership; replaces */
json *json_obj_get(const json *obj, const char *key);   /* NULL if absent */

void json_serialize(const json *j, sbuf *out);

/* Parses exactly one value spanning [s, s+len) (trailing whitespace ok).
 * Returns NULL and sets *err (static string) on failure. Nesting is limited
 * so a hostile peer cannot overflow the stack. */
json *json_parse(const char *s, size_t len, const char **err);

#endif
