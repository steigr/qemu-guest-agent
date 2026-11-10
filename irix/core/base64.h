/* base64.h - RFC 4648 base64, used by the file and exec command families. */
#ifndef QGA_BASE64_H
#define QGA_BASE64_H

#include "util.h"

void base64_encode(const unsigned char *in, size_t n, sbuf *out);

/* Returns 0 on success, -1 on invalid input (out contents then undefined).
 * Whitespace is rejected; padding is required. */
int base64_decode(const char *in, size_t n, sbuf *out);

#endif
