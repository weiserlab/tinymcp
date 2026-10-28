/*
 * tinymcp v0.8
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp_http.c - a POST's header lines and body read a byte at a time,
 * the 2026-07-28 header rules checked against the body, and each reply
 * written as JSON or, once a notification comes first, as an event stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tinymcp_http.h"

#include <string.h>

#define META_PV   "io.modelcontextprotocol/protocolVersion"

#define F_POST       0x0001u
#define F_PATH       0x0002u
#define F_ACC_JSON   0x0004u
#define F_ACC_SSE    0x0008u
#define F_CTYPE      0x0010u
#define F_ORIGIN_BAD 0x0020u
#define F_LENGTH     0x0040u
#define F_CHUNKED    0x0080u
#define F_LONG       0x0100u  /* a header line longer than the line buffer */
#define F_CLOSE      0x0200u  /* the client asked to close, or spoke HTTP/1.0 */
#define F_BAD        0x0400u  /* a request line or header that does not parse */

#define R_NONE 0                  /* nothing of the response written yet */
#define R_JSON 1                  /* application/json in chunks */
#define R_SSE  2                  /* text/event-stream in chunks */

#define P_NONE 0
#define P_NOTE 1                  /* a notification is going out in pieces */
#define P_RESP 2                  /* the response is going out in pieces */

#define EVENT_HEAD "event: message\r\ndata: "
#define EVENT_TAIL "\r\n\r\n"

static const char resp_prefix[] = "{\"jsonrpc\":\"2.0\",\"id\":";

/* Writing */

static void put(tinymcp_http_t *h, const char *s, size_t n)
{
    if (n)
        h->write(h->ctx, s, n);
}

static void puts_(tinymcp_http_t *h, const char *s)
{
    put(h, s, strlen(s));
}

static void put_num(tinymcp_http_t *h, unsigned long v, unsigned base)
{
    static const char digits[] = "0123456789abcdef";
    char d[24];
    size_t i = sizeof d;

    do {
        d[--i] = digits[v % base];
        v /= base;
    } while (v);
    put(h, d + i, sizeof d - i);
}

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 415: return "Unsupported Media Type";
    default:  return "Request Header Fields Too Large";
    }
}

/* The status line and headers; length < 0 means chunked. */
static void head(tinymcp_http_t *h, int status, const char *ctype, long length,
                 const char *extra)
{
    puts_(h, "HTTP/1.1 ");
    put_num(h, (unsigned long)status, 10);
    puts_(h, " ");
    puts_(h, reason(status));
    puts_(h, "\r\n");
    if (ctype) {
        puts_(h, "Content-Type: ");
        puts_(h, ctype);
        puts_(h, "\r\n");
    }
    if (length < 0) {
        puts_(h, "Transfer-Encoding: chunked\r\n");
    } else {
        puts_(h, "Content-Length: ");
        put_num(h, (unsigned long)length, 10);
        puts_(h, "\r\n");
    }
    if (h->flags & F_CLOSE)
        puts_(h, "Connection: close\r\n");
    if (extra)
        puts_(h, extra);
    puts_(h, "\r\n");
}

static void chunk(tinymcp_http_t *h, const char *s, size_t n)
{
    if (!n)
        return;
    put_num(h, (unsigned long)n, 16);
    puts_(h, "\r\n");
    put(h, s, n);
    puts_(h, "\r\n");
}

/* The response is complete: ready for the next request, or to close. */
static void finish(tinymcp_http_t *h)
{
    h->write(h->ctx, NULL, 0);
    h->state = (h->flags & F_CLOSE) ? TINYMCP_HTTP_S_CLOSE
                                    : TINYMCP_HTTP_S_LINE;
    h->resp = R_NONE;
    h->part = P_NONE;
    h->line_len = 0;
}

/* A response with no body; extra headers may be NULL. */
static void plain(tinymcp_http_t *h, int status, const char *extra)
{
    head(h, status, NULL, 0, extra);
    finish(h);
}

/*
 * A refusal before the body: read past the body first, so the connection
 * stays usable and the client is not reset while it still sends.
 */
static void refuse(tinymcp_http_t *h, int status)
{
    if (!h->body_len) {
        plain(h, status, status == 405 ? "Allow: POST\r\n" : NULL);
        return;
    }
    h->skip = (unsigned short)status;
    h->state = TINYMCP_HTTP_S_BODY;
    h->body_got = 0;
}

/* A JSON-RPC error as the whole response; id is raw JSON or NULL. */
static void rpc_error(tinymcp_http_t *h, int status, const tinymcp_jv_t *id,
                      long code, const char *message)
{
    tinymcp_jw_t w;

    tinymcp_jw_init(&w, h->line, sizeof h->line);
    tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id && id->type == TINYMCP_J_STRING) {
        tinymcp_jw_rawn(&w, "\"", 1);
        tinymcp_jw_rawn(&w, id->at, id->len);
        tinymcp_jw_rawn(&w, "\"", 1);
    } else if (id && id->type == TINYMCP_J_NUMBER) {
        tinymcp_jw_rawn(&w, id->at, id->len);
    } else {
        tinymcp_jw_raw(&w, "null");
    }
    tinymcp_jw_raw(&w, ",\"error\":{\"code\":");
    tinymcp_jw_long(&w, code);
    tinymcp_jw_key(&w, "message");
    tinymcp_jw_str(&w, message);
    tinymcp_jw_raw(&w, "}}");
    if (w.full) {               /* an id too long for the line: none */
        rpc_error(h, status, NULL, code, message);
        return;
    }
    head(h, status, "application/json", (long)w.len, NULL);
    put(h, w.buf, w.len);
    finish(h);
}

/* The HTTP status for a reply: the spec's table for an error, else 200. */
static int status_of(const char *msg, size_t len)
{
    static const char key[] = "\"error\":{\"code\":";
    size_t k = sizeof key - 1, i;
    long code = 0;

    for (i = 0; i + k < len && i < 160; i++) {
        if (memcmp(msg + i, key, k))
            continue;
        i += k;
        if (i < len && msg[i] == '-')
            i++;
        while (i < len && msg[i] >= '0' && msg[i] <= '9')
            code = code * 10 + (msg[i++] - '0');
        switch (code) {
        case 32700: case 32600: case 32602:
        case 32020: case 32021: case 32022:
            return 400;
        case 32601:
            return 404;
        default:
            return 200;
        }
    }
    return 200;
}

static int is_response(const char *msg, size_t len)
{
    return len >= sizeof resp_prefix - 1 &&
           !memcmp(msg, resp_prefix, sizeof resp_prefix - 1);
}

static void commit_sse(tinymcp_http_t *h)
{
    head(h, 200, "text/event-stream", -1, "Cache-Control: no-cache\r\n");
    h->resp = R_SSE;
}

/* One whole message as one event, in one chunk. */
static void event(tinymcp_http_t *h, const char *msg, size_t len)
{
    put_num(h, (unsigned long)(len + sizeof EVENT_HEAD - 1 +
                               sizeof EVENT_TAIL - 1), 16);
    puts_(h, "\r\n" EVENT_HEAD);
    put(h, msg, len);
    puts_(h, EVENT_TAIL "\r\n");
}

int tinymcp_http_send(tinymcp_http_t *h, const char *msg, size_t len)
{
    int resp = is_response(msg, len);

    if (h->state != TINYMCP_HTTP_S_OWED)
        return -1;
    if (h->resp == R_NONE && resp) {
        head(h, status_of(msg, len), "application/json", (long)len, NULL);
        put(h, msg, len);
        finish(h);
        return 0;
    }
    if (h->resp == R_NONE)
        commit_sse(h);
    event(h, msg, len);
    if (resp) {
        puts_(h, "0\r\n\r\n");
        finish(h);
    } else {
        h->write(h->ctx, NULL, 0);
    }
    return 0;
}

int tinymcp_http_send_part(tinymcp_http_t *h, const char *bytes, size_t n,
                           int last)
{
    if (h->state != TINYMCP_HTTP_S_OWED)
        return -1;
    if (h->part == P_NONE) {
        h->part = is_response(bytes, n) ? P_RESP : P_NOTE;
        if (h->resp == R_NONE && h->part == P_RESP) {
            head(h, 200, "application/json", -1, NULL);
            h->resp = R_JSON;
        } else if (h->resp == R_NONE) {
            commit_sse(h);
        }
        if (h->resp == R_SSE)
            chunk(h, EVENT_HEAD, sizeof EVENT_HEAD - 1);
    }
    chunk(h, bytes, n);
    if (!last)
        return 0;
    if (h->resp == R_SSE)
        chunk(h, EVENT_TAIL, sizeof EVENT_TAIL - 1);
    if (h->part == P_RESP) {
        puts_(h, "0\r\n\r\n");
        finish(h);
    } else {
        h->part = P_NONE;
        h->write(h->ctx, NULL, 0);
    }
    return 0;
}

/* Reading */

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* s starts with the lowercase prefix p, case ignored. */
static int starts(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (lower((unsigned char)*s) != *p)
            return 0;
    return 1;
}

/* s holds the lowercase token p anywhere, case ignored. */
static int holds(const char *s, const char *p)
{
    for (; *s; s++)
        if (starts(s, p))
            return 1;
    return 0;
}

/* A routing header's value; one past the field marks it unusable. */
static void field(tinymcp_http_t *h, char *dst, size_t cap, const char *v)
{
    size_t n = strlen(v);

    if (dst[0])
        h->dup = 1;
    if (n + 1 > cap) {
        strcpy(dst, "\x01");    /* matches no body value */
        return;
    }
    memcpy(dst, v, n + 1);
}

static void origin(tinymcp_http_t *h, const char *v)
{
    const char *const *o;

    for (o = h->origins; o && *o; o++)
        if (!strcmp(*o, v))
            return;
    h->flags |= F_ORIGIN_BAD;
}

static void header(tinymcp_http_t *h, char *line)
{
    char *colon = strchr(line, ':');
    char *v, *e;

    if (!colon) {
        h->flags |= F_BAD;
        return;
    }
    *colon = 0;
    for (v = colon + 1; *v == ' ' || *v == '\t'; v++)
        ;
    for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t'); e--)
        ;
    *e = 0;
    if (starts(line, "content-length") && !line[14]) {
        size_t n = 0;

        if (!*v)
            h->flags |= F_BAD;
        for (; *v; v++) {
            if (*v < '0' || *v > '9' || n > ((size_t)-1) / 10 - 9) {
                h->flags |= F_BAD;
                return;
            }
            n = n * 10 + (size_t)(*v - '0');
        }
        h->body_len = n;
        h->flags |= F_LENGTH;
    } else if (starts(line, "transfer-encoding") && !line[17]) {
        h->flags |= F_CHUNKED;
    } else if (starts(line, "content-type") && !line[12]) {
        if (starts(v, "application/json"))
            h->flags |= F_CTYPE;
    } else if (starts(line, "accept") && !line[6]) {
        if (holds(v, "application/json") || holds(v, "*/*"))
            h->flags |= F_ACC_JSON;
        if (holds(v, "text/event-stream") || holds(v, "*/*"))
            h->flags |= F_ACC_SSE;
    } else if (starts(line, "origin") && !line[6]) {
        origin(h, v);
    } else if (starts(line, "mcp-protocol-version") && !line[20]) {
        field(h, h->version, sizeof h->version, v);
    } else if (starts(line, "mcp-method") && !line[10]) {
        field(h, h->method, sizeof h->method, v);
    } else if (starts(line, "mcp-name") && !line[8]) {
        field(h, h->name, sizeof h->name, v);
    } else if (starts(line, "authorization") && !line[13]) {
        if (starts(v, "bearer ") && strlen(v + 7) < sizeof h->bearer)
            strcpy(h->bearer, v + 7);
    } else if (starts(line, "connection") && !line[10]) {
        if (holds(v, "close"))
            h->flags |= F_CLOSE;
    }
}

static void request_line(tinymcp_http_t *h, char *line)
{
    char *target = strchr(line, ' ');
    char *version, *q;

    h->flags = 0;
    h->dup = 0;
    h->skip = 0;
    h->body_len = 0;
    h->body_got = 0;
    h->version[0] = h->method[0] = h->name[0] = h->bearer[0] = 0;
    if (!target) {
        h->flags |= F_BAD;
        return;
    }
    *target++ = 0;
    version = strchr(target, ' ');
    if (!version) {
        h->flags |= F_BAD;
        return;
    }
    *version++ = 0;
    if (!strcmp(line, "POST"))
        h->flags |= F_POST;
    q = strchr(target, '?');
    if (q)
        *q = 0;
    if (!strcmp(target, h->path))
        h->flags |= F_PATH;
    if (!strcmp(version, "HTTP/1.0"))
        h->flags |= F_CLOSE;
    else if (strcmp(version, "HTTP/1.1"))
        h->flags |= F_BAD;
}

/* The base64 of a "=?base64?...?=" header, decoded in place; 0, or -1. */
static int unwrap(char *s)
{
    size_t n = strlen(s), i, o = 0;
    unsigned acc = 0, bits = 0;

    if (n < 12 || strncmp(s, "=?base64?", 9) || strcmp(s + n - 2, "?="))
        return 0;
    for (i = 9; i < n - 2; i++) {
        int c = (unsigned char)s[i], d;

        if (c >= 'A' && c <= 'Z')
            d = c - 'A';
        else if (c >= 'a' && c <= 'z')
            d = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            d = c - '0' + 52;
        else if (c == '+')
            d = 62;
        else if (c == '/')
            d = 63;
        else if (c == '=')
            break;
        else
            return -1;
        acc = (acc << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            s[o++] = (char)((acc >> bits) & 0xFF);
        }
    }
    s[o] = 0;
    return 0;
}

/* A string value, decoded in a copy, equals s. */
static int same(const tinymcp_jv_t *v, const char *s)
{
    char copy[TINYMCP_URI_MAX + 32];
    int n;

    if (v->type != TINYMCP_J_STRING || v->len + 1 > sizeof copy)
        return 0;
    memcpy(copy, v->at, v->len);
    n = tinymcp_json_decode(copy, v->len);
    return n >= 0 && !strcmp(copy, s);
}

/* The body is in; check it against the headers and hand it over. */
static void serve(tinymcp_http_t *h)
{
    tinymcp_t *t = h->t;
    char *body = t->in;
    size_t n = h->body_len;
    tinymcp_jv_t root, id, method, params, meta, pv, v;
    const char *name_key = NULL;

    body[n] = 0;
    if (!tinymcp_json_value(body, n, &root) || root.type != TINYMCP_J_OBJECT) {
        rpc_error(h, 400, NULL, -32700, "parse error");
        return;
    }
    if (tinymcp_json_get(&root, "method", &method) != 1 ||
        method.type != TINYMCP_J_STRING) {
        rpc_error(h, 400, NULL, -32600,
                  "the body must be one JSON-RPC request or notification");
        return;
    }
    if (tinymcp_json_get(&root, "id", &id) != 1) {
        /* This revision has no client notifications over HTTP: dropped. */
        plain(h, 202, NULL);
        return;
    }
    if (tinymcp_json_get(&root, "params", &params) != 1 ||
        params.type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(&params, "_meta", &meta) != 1 ||
        meta.type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(&meta, META_PV, &pv) != 1) {
        /* The server checks the rest of the envelope itself. */
        rpc_error(h, 400, &id, -32602, "params._meta must carry the protocol "
                  "version: this endpoint serves 2026-07-28");
        return;
    }
    if (same(&method, "tools/call") || same(&method, "prompts/get"))
        name_key = "name";
    else if (same(&method, "resources/read"))
        name_key = "uri";
    if (h->dup) {
        rpc_error(h, 400, &id, -32020, "a routing header appears more than once");
        return;
    }
    if (!h->version[0] || !same(&pv, h->version)) {
        rpc_error(h, 400, &id, -32020, "MCP-Protocol-Version does not match "
                  "the body's protocol version");
        return;
    }
    if (!same(&method, h->method)) {
        rpc_error(h, 400, &id, -32020, "Mcp-Method does not match the body's "
                  "method");
        return;
    }
    if (name_key && tinymcp_json_get(&params, name_key, &v) == 1 &&
        v.type == TINYMCP_J_STRING &&
        (unwrap(h->name) < 0 || !same(&v, h->name))) {
        rpc_error(h, 400, &id, -32020, "Mcp-Name does not match the body");
        return;
    }
    h->state = TINYMCP_HTTP_S_OWED;
    h->resp = R_NONE;
    h->part = P_NONE;
    tinymcp_handle(t, body, n);
}

/* The header lines are done: refuse the request, or read its body. */
static void begin(tinymcp_http_t *h)
{
    if (h->flags & (F_BAD | F_LONG)) {
        h->flags |= F_CLOSE;
        plain(h, (h->flags & F_LONG) ? 431 : 400, NULL);
    } else if ((h->flags & F_CHUNKED) ||
               (!(h->flags & F_LENGTH) && (h->flags & F_POST))) {
        /* No length to read past: the connection ends with the refusal. */
        h->flags |= F_CLOSE;
        plain(h, 411, NULL);
    } else if (!(h->flags & F_PATH)) {
        refuse(h, 404);
    } else if (!(h->flags & F_POST)) {
        refuse(h, 405);
    } else if (h->flags & F_ORIGIN_BAD) {
        refuse(h, 403);
    } else if ((h->flags & (F_ACC_JSON | F_ACC_SSE)) !=
               (F_ACC_JSON | F_ACC_SSE)) {
        refuse(h, 406);
    } else if (!(h->flags & F_CTYPE)) {
        refuse(h, 415);
    } else if (h->body_len + 1 > h->t->in_cap) {
        refuse(h, 413);
    } else {
        h->state = TINYMCP_HTTP_S_BODY;
        h->body_got = 0;
        if (!h->body_len)
            serve(h);
    }
}

static void end_line(tinymcp_http_t *h)
{
    char *line = h->line;

    if (h->line_len && line[h->line_len - 1] == '\r')
        h->line_len--;
    line[h->line_len] = 0;
    h->line_len = 0;
    if (h->state == TINYMCP_HTTP_S_LINE) {
        if (!line[0])
            return;             /* a blank line before a request is allowed */
        request_line(h, line);
        h->state = TINYMCP_HTTP_S_HEAD;
    } else if (!line[0]) {
        begin(h);
    } else {
        header(h, line);
    }
}

void tinymcp_http_init(tinymcp_http_t *h, tinymcp_t *t, const char *path,
                       tinymcp_write_fn write, void *ctx)
{
    memset(h, 0, sizeof *h);
    h->t = t;
    h->path = path;
    h->write = write;
    h->ctx = ctx;
}

size_t tinymcp_http_feed(tinymcp_http_t *h, const char *bytes, size_t n)
{
    size_t i = 0;

    while (i < n && h->state <= TINYMCP_HTTP_S_BODY) {
        char c;

        if (h->state == TINYMCP_HTTP_S_BODY) {
            size_t take = n - i;

            if (take > h->body_len - h->body_got)
                take = h->body_len - h->body_got;
            if (!h->skip)
                memcpy(h->t->in + h->body_got, bytes + i, take);
            h->body_got += take;
            i += take;
            if (h->body_got < h->body_len)
                continue;
            if (h->skip) {
                int status = h->skip;

                h->skip = 0;
                plain(h, status, status == 405 ? "Allow: POST\r\n" : NULL);
            } else {
                serve(h);
            }
            continue;
        }
        c = bytes[i++];
        if (c == '\n')
            end_line(h);
        else if (h->line_len + 1 < sizeof h->line)
            h->line[h->line_len++] = c;
        else
            h->flags |= F_LONG;
    }
    return i;
}

const char *tinymcp_http_bearer(const tinymcp_http_t *h)
{
    return h->bearer;
}
