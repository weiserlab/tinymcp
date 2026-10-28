/*
 * tinymcp v0.8
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp_http.h - Streamable HTTP for the 2026-07-28 revision: one
 * connection's requests parsed and checked, its server's replies written
 * back as JSON or as an event stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TINYMCP_HTTP_H
#define TINYMCP_HTTP_H

#include "tinymcp.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TINYMCP_HTTP_LINE
#define TINYMCP_HTTP_LINE 256    /* the longest header line taken (431 past it) */
#endif
#ifndef TINYMCP_HTTP_FIELD
#define TINYMCP_HTTP_FIELD 64    /* a routing header or bearer token kept */
#endif

/*
 * Bytes onto the connection; NULL and 0 mark the end of a response or of
 * an event, where a host that buffers sends what it holds.
 */
typedef int (*tinymcp_write_fn)(void *ctx, const char *bytes, size_t n);

/** One HTTP connection in front of one tinymcp_t. */
typedef struct tinymcp_http {
    tinymcp_t *t;               /* its in buffer holds a request's body */
    tinymcp_write_fn write;
    void *ctx;
    const char *path;           /* the endpoint, such as "/mcp" */
    const char *const *origins; /* Origin values allowed, NULL-ended; NULL: none */
    unsigned char state;        /* the parser's TINYMCP_HTTP_S_* */
    unsigned char resp;         /* how the owed response is being written */
    unsigned char part;         /* a message is going out in pieces */
    unsigned char dup;          /* a routing header came twice */
    unsigned short skip;        /* the refusal owed once the body is read past */
    unsigned flags;
    size_t line_len;
    size_t body_len;
    size_t body_got;
    char line[TINYMCP_HTTP_LINE];
    char version[TINYMCP_HTTP_FIELD];
    char method[TINYMCP_HTTP_FIELD];
    char name[TINYMCP_URI_MAX + 32];
    char bearer[TINYMCP_HTTP_FIELD];
} tinymcp_http_t;

#define TINYMCP_HTTP_S_LINE  0  /* waiting for a request line */
#define TINYMCP_HTTP_S_HEAD  1  /* reading header lines */
#define TINYMCP_HTTP_S_BODY  2  /* reading Content-Length bytes, or past them */
#define TINYMCP_HTTP_S_OWED  3  /* a request is being served; its response is owed */
#define TINYMCP_HTTP_S_CLOSE 4  /* the host closes the connection */

/**
 * Bind a connection to its server.  The server's send and send_part must
 * reach tinymcp_http_send() and tinymcp_http_send_part() for it.
 */
void tinymcp_http_init(tinymcp_http_t *h, tinymcp_t *t, const char *path,
                       tinymcp_write_fn write, void *ctx);

/**
 * Bytes from the connection.  A request whose body is complete is checked
 * and handed to the server before this returns.
 * @return the bytes taken: fewer than n while a response is owed, when the
 *         host keeps the rest for after it.
 */
size_t tinymcp_http_feed(tinymcp_http_t *h, const char *bytes, size_t n);

/** The server's message for this connection, whole or in pieces. */
int tinymcp_http_send(tinymcp_http_t *h, const char *msg, size_t len);
int tinymcp_http_send_part(tinymcp_http_t *h, const char *bytes, size_t n,
                           int last);

/** The request's Authorization bearer token, "" for none. */
const char *tinymcp_http_bearer(const tinymcp_http_t *h);

#ifdef __cplusplus
}
#endif

#endif /* TINYMCP_HTTP_H */
