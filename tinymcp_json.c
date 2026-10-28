/*
 * tinymcp v0.4
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp_json.c - the scanner locates values without copying them, the
 * decoder shrinks a string over itself, and the writer refuses what does not
 * fit instead of truncating a token.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tinymcp_json.h"

#include <string.h>

static size_t skip_ws(const char *s, size_t n)
{
    size_t i = 0;

    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                     s[i] == '\r'))
        i++;
    return i;
}

/* s[0] is the opening quote; the bytes past the closing one, or 0. */
static size_t scan_string(const char *s, size_t n)
{
    size_t i = 1;

    while (i < n) {
        if (s[i] == '"')
            return i + 1;
        if (s[i] == '\\')
            i += 2;
        else
            i++;
    }
    return 0;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static size_t scan_number(const char *s, size_t n)
{
    size_t i = 0, d;

    if (i < n && s[i] == '-')
        i++;
    d = i;
    while (i < n && (is_digit(s[i]) || s[i] == '.' || s[i] == 'e' ||
                     s[i] == 'E' || s[i] == '+' || s[i] == '-'))
        i++;
    return i > d ? i : 0;
}

/* s[0] opens an object or array; the bytes past its close, or 0. */
static size_t scan_container(const char *s, size_t n)
{
    size_t i = 1;
    int depth = 1;

    while (i < n && depth > 0) {
        char c = s[i];

        if (c == '"') {
            size_t k = scan_string(s + i, n - i);

            if (!k)
                return 0;
            i += k;
            continue;
        }
        if (c == '{' || c == '[')
            depth++;
        else if (c == '}' || c == ']')
            depth--;
        i++;
    }
    return depth == 0 ? i : 0;
}

size_t tinymcp_json_value(const char *s, size_t n, tinymcp_jv_t *out)
{
    size_t i = skip_ws(s, n), k = 0;

    out->type = TINYMCP_J_NONE;
    out->at = s + i;
    out->len = 0;
    if (i >= n)
        return 0;
    switch (s[i]) {
    case '{':
        k = scan_container(s + i, n - i);
        out->type = TINYMCP_J_OBJECT;
        out->len = k;
        break;
    case '[':
        k = scan_container(s + i, n - i);
        out->type = TINYMCP_J_ARRAY;
        out->len = k;
        break;
    case '"':
        k = scan_string(s + i, n - i);
        out->type = TINYMCP_J_STRING;
        out->at = s + i + 1;
        out->len = k >= 2 ? k - 2 : 0;
        break;
    case 't':
        if (n - i >= 4 && !memcmp(s + i, "true", 4))
            k = 4;
        out->type = TINYMCP_J_TRUE;
        out->len = k;
        break;
    case 'f':
        if (n - i >= 5 && !memcmp(s + i, "false", 5))
            k = 5;
        out->type = TINYMCP_J_FALSE;
        out->len = k;
        break;
    case 'n':
        if (n - i >= 4 && !memcmp(s + i, "null", 4))
            k = 4;
        out->type = TINYMCP_J_NULL;
        out->len = k;
        break;
    default:
        k = scan_number(s + i, n - i);
        out->type = TINYMCP_J_NUMBER;
        out->len = k;
        break;
    }
    if (!k) {
        out->type = TINYMCP_J_NONE;
        return 0;
    }
    return i + k;
}

int tinymcp_json_next(const tinymcp_jv_t *obj, size_t *pos,
                      tinymcp_jv_t *key, tinymcp_jv_t *val)
{
    const char *s = obj->at;
    size_t n = obj->len, i = *pos ? *pos : 1, k;

    if (obj->type != TINYMCP_J_OBJECT || n < 2)
        return -1;
    i += skip_ws(s + i, n - i);
    if (i >= n)
        return -1;
    if (s[i] == '}') {
        *pos = i;
        return 0;
    }
    if (s[i] == ',') {
        i++;
        i += skip_ws(s + i, n - i);
    }
    if (i >= n || s[i] != '"')
        return -1;
    k = scan_string(s + i, n - i);
    if (!k)
        return -1;
    key->at = s + i + 1;
    key->len = k - 2;
    key->type = TINYMCP_J_STRING;
    i += k;
    i += skip_ws(s + i, n - i);
    if (i >= n || s[i] != ':')
        return -1;
    i++;
    k = tinymcp_json_value(s + i, n - i, val);
    if (!k)
        return -1;
    *pos = i + k;
    return 1;
}

int tinymcp_json_item(const tinymcp_jv_t *arr, size_t *pos, tinymcp_jv_t *val)
{
    const char *s = arr->at;
    size_t n = arr->len, i = *pos ? *pos : 1, k;

    if (arr->type != TINYMCP_J_ARRAY || n < 2)
        return -1;
    i += skip_ws(s + i, n - i);
    if (i >= n)
        return -1;
    if (s[i] == ']') {
        *pos = i;
        return 0;
    }
    if (s[i] == ',')
        i++;
    k = tinymcp_json_value(s + i, n - i, val);
    if (!k)
        return -1;
    *pos = i + k;
    return 1;
}

int tinymcp_json_get(const tinymcp_jv_t *obj, const char *key,
                     tinymcp_jv_t *out)
{
    size_t pos = 0;
    tinymcp_jv_t k, v;
    int rc;

    while ((rc = tinymcp_json_next(obj, &pos, &k, &v)) == 1) {
        if (tinymcp_json_is(&k, key)) {
            *out = v;
            return 1;
        }
    }
    return rc;
}

int tinymcp_json_is(const tinymcp_jv_t *v, const char *lit)
{
    size_t n = strlen(lit);

    return v->len == n && !memcmp(v->at, lit, n);
}

long tinymcp_json_long(const tinymcp_jv_t *v)
{
    size_t i = 0;
    long r = 0;
    int neg = 0;

    if (v->type != TINYMCP_J_NUMBER)
        return 0;
    if (i < v->len && v->at[i] == '-') {
        neg = 1;
        i++;
    }
    while (i < v->len && is_digit(v->at[i]))
        r = r * 10 + (v->at[i++] - '0');
    return neg ? -r : r;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0, i;

    for (i = 0; i < 4; i++) {
        char c = s[i];

        v <<= 4;
        if (is_digit(c))
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

static size_t utf8_put(char *d, unsigned cp)
{
    if (cp < 0x80) {
        d[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        d[0] = (char)(0xC0 | (cp >> 6));
        d[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        d[0] = (char)(0xE0 | (cp >> 12));
        d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        d[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    d[0] = (char)(0xF0 | (cp >> 18));
    d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    d[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    d[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int tinymcp_json_decode(char *s, size_t n)
{
    size_t r = 0, w = 0;

    while (r < n) {
        char c = s[r], e;
        unsigned cp, lo;

        if (c != '\\') {
            s[w++] = c;
            r++;
            continue;
        }
        if (r + 1 >= n)
            return -1;
        e = s[r + 1];
        r += 2;
        switch (e) {
        case '"':
        case '\\':
        case '/':
            s[w++] = e;
            break;
        case 'b':
            s[w++] = '\b';
            break;
        case 'f':
            s[w++] = '\f';
            break;
        case 'n':
            s[w++] = '\n';
            break;
        case 'r':
            s[w++] = '\r';
            break;
        case 't':
            s[w++] = '\t';
            break;
        case 'u':
            if (r + 4 > n || hex4(s + r, &cp) < 0)
                return -1;
            r += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (r + 6 <= n && s[r] == '\\' && s[r + 1] == 'u' &&
                    hex4(s + r + 2, &lo) == 0 && lo >= 0xDC00 &&
                    lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    r += 6;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            w += utf8_put(s + w, cp);
            break;
        default:
            return -1;
        }
    }
    s[w] = 0;
    return (int)w;
}

/* The writer. */

void tinymcp_jw_init(tinymcp_jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->full = 0;
    if (cap)
        buf[0] = 0;
}

void tinymcp_jw_rawn(tinymcp_jw_t *w, const char *s, size_t n)
{
    if (w->full)
        return;
    if (w->len + n >= w->cap) {
        w->full = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

void tinymcp_jw_raw(tinymcp_jw_t *w, const char *s)
{
    tinymcp_jw_rawn(w, s, strlen(s));
}

/* The escaped form of c: its length, and the bytes in e[0..6). */
static size_t escaped(char c, char *e)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char u = (unsigned char)c;

    switch (c) {
    case '"':
        e[0] = '\\';
        e[1] = '"';
        return 2;
    case '\\':
        e[0] = '\\';
        e[1] = '\\';
        return 2;
    case '\n':
        e[0] = '\\';
        e[1] = 'n';
        return 2;
    case '\r':
        e[0] = '\\';
        e[1] = 'r';
        return 2;
    case '\t':
        e[0] = '\\';
        e[1] = 't';
        return 2;
    case '\b':
        e[0] = '\\';
        e[1] = 'b';
        return 2;
    case '\f':
        e[0] = '\\';
        e[1] = 'f';
        return 2;
    default:
        break;
    }
    if (u < 0x20) {
        e[0] = '\\';
        e[1] = 'u';
        e[2] = '0';
        e[3] = '0';
        e[4] = hex[u >> 4];
        e[5] = hex[u & 15];
        return 6;
    }
    e[0] = c;
    return 1;
}

void tinymcp_jw_strn(tinymcp_jw_t *w, const char *s, size_t n)
{
    size_t mark = w->len, i;
    char e[6];

    if (w->full)
        return;
    tinymcp_jw_rawn(w, "\"", 1);
    for (i = 0; i < n && !w->full; i++)
        tinymcp_jw_rawn(w, e, escaped(s[i], e));
    tinymcp_jw_rawn(w, "\"", 1);
    if (w->full) {
        tinymcp_jw_reset(w, mark);
        w->full = 1;
    }
}

void tinymcp_jw_str(tinymcp_jw_t *w, const char *s)
{
    tinymcp_jw_strn(w, s, strlen(s));
}

void tinymcp_jw_ulong(tinymcp_jw_t *w, unsigned long v)
{
    char d[24];
    size_t i = sizeof d;

    do {
        d[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    tinymcp_jw_rawn(w, d + i, sizeof d - i);
}

void tinymcp_jw_long(tinymcp_jw_t *w, long v)
{
    if (v < 0) {
        tinymcp_jw_rawn(w, "-", 1);
        tinymcp_jw_ulong(w, 0UL - (unsigned long)v);
    } else {
        tinymcp_jw_ulong(w, (unsigned long)v);
    }
}

void tinymcp_jw_sep(tinymcp_jw_t *w)
{
    char c;

    if (w->len == 0)
        return;
    c = w->buf[w->len - 1];
    if (c == '{' || c == '[' || c == ':')
        return;
    tinymcp_jw_rawn(w, ",", 1);
}

void tinymcp_jw_key(tinymcp_jw_t *w, const char *key)
{
    tinymcp_jw_sep(w);
    tinymcp_jw_str(w, key);
    tinymcp_jw_rawn(w, ":", 1);
}

void tinymcp_jw_reset(tinymcp_jw_t *w, size_t mark)
{
    if (mark < w->len) {
        w->len = mark;
        w->buf[mark] = 0;
    }
    w->full = 0;
}

/* The sink. */

void tinymcp_sink_init(tinymcp_sink_t *s, tinymcp_jw_t *w, size_t limit)
{
    s->w = w;
    s->limit = limit;
    s->cut = 0;
}

void tinymcp_sink_putc(tinymcp_sink_t *s, char c)
{
    char e[6];
    size_t k;

    if (s->cut)
        return;
    k = escaped(c, e);
    if (s->w->len + k > s->limit || s->w->len + k >= s->w->cap) {
        s->cut = 1;
        return;
    }
    tinymcp_jw_rawn(s->w, e, k);
}

int tinymcp_sink_write(tinymcp_sink_t *s, const char *text, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        tinymcp_sink_putc(s, text[i]);
    return s->cut ? TINYMCP_JSON_EFULL : 0;
}

int tinymcp_sink_puts(tinymcp_sink_t *s, const char *text)
{
    return tinymcp_sink_write(s, text, strlen(text));
}
