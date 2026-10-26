/*
 * tinymcp v0.3
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp_json.h - JSON for one message in one buffer: a scanner over the
 * bytes as they arrived, in-place string decoding, and a bounded writer with
 * an escaping character sink.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TINYMCP_JSON_H
#define TINYMCP_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tinymcp_jtype {
    TINYMCP_J_NONE = 0,
    TINYMCP_J_OBJECT,
    TINYMCP_J_ARRAY,
    TINYMCP_J_STRING,
    TINYMCP_J_NUMBER,
    TINYMCP_J_TRUE,
    TINYMCP_J_FALSE,
    TINYMCP_J_NULL
} tinymcp_jtype_t;

/**
 * One value located in the message: for a string, the raw bytes between
 * its quotes with the escapes still in place; for anything else, the whole
 * token.  The message is not copied.
 */
typedef struct tinymcp_jv {
    const char *at;
    size_t len;
    tinymcp_jtype_t type;
} tinymcp_jv_t;

/**
 * Scan one value at s[0..n) after any leading whitespace.
 * @return the bytes consumed, or 0 when no well-formed value starts there.
 */
size_t tinymcp_json_value(const char *s, size_t n, tinymcp_jv_t *out);

/**
 * Look a member up by its raw key in an object value.
 * @return 1 found, 0 absent, -1 when the object is malformed.
 */
int tinymcp_json_get(const tinymcp_jv_t *obj, const char *key,
                     tinymcp_jv_t *out);

/**
 * Step through an object's members; *pos starts at 0 and is advanced.
 * @return 1 with the key (raw) and its value, 0 at the end, -1 malformed.
 */
int tinymcp_json_next(const tinymcp_jv_t *obj, size_t *pos,
                      tinymcp_jv_t *key, tinymcp_jv_t *val);

/**
 * Step through an array's items; *pos starts at 0 and is advanced.
 * @return 1 with the item, 0 at the end, -1 malformed.
 */
int tinymcp_json_item(const tinymcp_jv_t *arr, size_t *pos, tinymcp_jv_t *val);

/**
 * Decode a raw string in place to UTF-8 and terminate it; every escape
 * shrinks the text, so the terminator lands at or before the closing quote.
 * The enclosing value must not be scanned again afterwards.
 * @return the decoded length, or -1 for an escape the grammar does not have.
 */
int tinymcp_json_decode(char *s, size_t n);

/** True when the raw string equals lit byte for byte, escapes untouched. */
int tinymcp_json_is(const tinymcp_jv_t *v, const char *lit);

/** A number token as a long; digits after a point or an exponent are dropped. */
long tinymcp_json_long(const tinymcp_jv_t *v);

/**
 * The writer: appends into buf, never past cap - 1, and keeps a terminator.
 * The first write that does not fit sets full and is dropped whole; what
 * was written before it stays.
 */
typedef struct tinymcp_jw {
    char *buf;
    size_t cap;
    size_t len;
    unsigned char full;
} tinymcp_jw_t;

void tinymcp_jw_init(tinymcp_jw_t *w, char *buf, size_t cap);
/** Verbatim bytes: structure, numbers the caller formatted, raw JSON. */
void tinymcp_jw_raw(tinymcp_jw_t *w, const char *s);
void tinymcp_jw_rawn(tinymcp_jw_t *w, const char *s, size_t n);
/** A quoted, escaped string. */
void tinymcp_jw_str(tinymcp_jw_t *w, const char *s);
void tinymcp_jw_strn(tinymcp_jw_t *w, const char *s, size_t n);
void tinymcp_jw_long(tinymcp_jw_t *w, long v);
void tinymcp_jw_ulong(tinymcp_jw_t *w, unsigned long v);
/** A comma unless the last byte opened an object or array or was a colon. */
void tinymcp_jw_sep(tinymcp_jw_t *w);
/** The separator, then "key": */
void tinymcp_jw_key(tinymcp_jw_t *w, const char *key);
/** Drop everything written since len was mark and clear full. */
void tinymcp_jw_reset(tinymcp_jw_t *w, size_t mark);

/**
 * An escaping text sink inside an open JSON string: a shell's putc can be
 * this.  Text past limit is dropped and cut is set; the JSON stays valid.
 */
typedef struct tinymcp_sink {
    tinymcp_jw_t *w;
    size_t limit;
    unsigned char cut;
} tinymcp_sink_t;

#define TINYMCP_JSON_EFULL (-4)   /* the text did not fit; the rest was cut */

void tinymcp_sink_init(tinymcp_sink_t *s, tinymcp_jw_t *w, size_t limit);
void tinymcp_sink_putc(tinymcp_sink_t *s, char c);
/** @return 0, or TINYMCP_JSON_EFULL once anything has been cut. */
int tinymcp_sink_write(tinymcp_sink_t *s, const char *text, size_t n);
int tinymcp_sink_puts(tinymcp_sink_t *s, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* TINYMCP_JSON_H */
