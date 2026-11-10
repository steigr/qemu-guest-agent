#include "base64.h"

static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const unsigned char *in, size_t n, sbuf *out)
{
    size_t k;

    for (k = 0; k + 2 < n; k += 3) {
        sb_putc(out, tbl[in[k] >> 2]);
        sb_putc(out, tbl[((in[k] & 3) << 4) | (in[k + 1] >> 4)]);
        sb_putc(out, tbl[((in[k + 1] & 15) << 2) | (in[k + 2] >> 6)]);
        sb_putc(out, tbl[in[k + 2] & 63]);
    }
    if (n - k == 1) {
        sb_putc(out, tbl[in[k] >> 2]);
        sb_putc(out, tbl[(in[k] & 3) << 4]);
        sb_puts(out, "==");
    } else if (n - k == 2) {
        sb_putc(out, tbl[in[k] >> 2]);
        sb_putc(out, tbl[((in[k] & 3) << 4) | (in[k + 1] >> 4)]);
        sb_putc(out, tbl[(in[k + 1] & 15) << 2]);
        sb_putc(out, '=');
    }
}

static int val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, size_t n, sbuf *out)
{
    size_t k;

    if (n % 4)
        return -1;
    for (k = 0; k < n; k += 4) {
        int a = val((unsigned char)in[k]);
        int b = val((unsigned char)in[k + 1]);
        int c, d;
        int last = (k + 4 == n);

        if (a < 0 || b < 0)
            return -1;
        if (last && in[k + 2] == '=') {
            if (in[k + 3] != '=' || (b & 15))
                return -1;
            sb_putc(out, (a << 2) | (b >> 4));
            return 0;
        }
        c = val((unsigned char)in[k + 2]);
        if (c < 0)
            return -1;
        if (last && in[k + 3] == '=') {
            if (c & 3)
                return -1;
            sb_putc(out, (a << 2) | (b >> 4));
            sb_putc(out, ((b & 15) << 4) | (c >> 2));
            return 0;
        }
        d = val((unsigned char)in[k + 3]);
        if (d < 0)
            return -1;
        sb_putc(out, (a << 2) | (b >> 4));
        sb_putc(out, ((b & 15) << 4) | (c >> 2));
        sb_putc(out, ((c & 3) << 6) | d);
    }
    return 0;
}
