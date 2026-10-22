/*
 * tinymcp v0.1
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp.c - one message in, one reply out.  A request carrying
 * _meta.protocolVersion is served statelessly; initialize switches the
 * stream to the handshake era; subscriptions live in the instance's slots.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tinymcp.h"

#include <string.h>

#define META_PV   "io.modelcontextprotocol/protocolVersion"
#define META_CAPS "io.modelcontextprotocol/clientCapabilities"
#define META_SUB  "io.modelcontextprotocol/subscriptionId"
#define META_SRV  "io.modelcontextprotocol/serverInfo"

/* Room kept after a page or a text for the closing envelope. */
#define TAIL_RESERVE 80

#define TTL_STATIC 3600000UL   /* discover, tools, templates: fixed for the boot */
#define TTL_LIVE   0UL         /* resources and their text: re-read each time */

static const char *const modern_versions[] = { "2026-07-28" };
static const char *const legacy_versions[] = {
    "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"
};
#define N_MODERN (sizeof modern_versions / sizeof modern_versions[0])
#define N_LEGACY (sizeof legacy_versions / sizeof legacy_versions[0])

typedef struct req {
    tinymcp_t *t;
    tinymcp_jv_t id;       /* TINYMCP_J_NONE for a notification */
    tinymcp_jv_t params;   /* TINYMCP_J_NONE when absent */
    const char *method;
    int modern;            /* _meta.protocolVersion present and supported */
    tinymcp_jw_t w;
} req_t;

/* Sending */

static void emit(tinymcp_t *t, tinymcp_jw_t *w)
{
    if (w->full || t->host->send(t->ctx, w->buf, w->len) != 0)
        t->dropped++;
}

static void write_id(tinymcp_jw_t *w, const tinymcp_jv_t *id)
{
    if (id->type == TINYMCP_J_STRING) {
        tinymcp_jw_rawn(w, "\"", 1);
        tinymcp_jw_rawn(w, id->at, id->len);
        tinymcp_jw_rawn(w, "\"", 1);
    } else if (id->type == TINYMCP_J_NONE) {
        tinymcp_jw_raw(w, "null");
    } else {
        tinymcp_jw_rawn(w, id->at, id->len);
    }
}

static void server_info(tinymcp_jw_t *w, const tinymcp_host_t *h)
{
    tinymcp_jw_raw(w, "{");
    tinymcp_jw_key(w, "name");
    tinymcp_jw_str(w, h->name);
    tinymcp_jw_key(w, "version");
    tinymcp_jw_str(w, h->version);
    tinymcp_jw_raw(w, "}");
}

static void begin_result(req_t *r)
{
    tinymcp_jw_t *w = &r->w;

    tinymcp_jw_reset(w, 0);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(w, &r->id);
    tinymcp_jw_raw(w, ",\"result\":{");
    if (r->modern) {
        tinymcp_jw_raw(w, "\"resultType\":\"complete\",\"_meta\":{\"" META_SRV
                          "\":");
        server_info(w, r->t->host);
        tinymcp_jw_raw(w, "}");
    }
}

static void error_reply(req_t *r, long code, const char *message);

static void end_result(req_t *r)
{
    tinymcp_jw_raw(&r->w, "}}");
    if (r->w.full) {
        error_reply(r, -32603, "reply does not fit the out buffer");
        return;
    }
    emit(r->t, &r->w);
}

static void cacheable(req_t *r, unsigned long ttl_ms)
{
    if (!r->modern)
        return;
    tinymcp_jw_key(&r->w, "ttlMs");
    tinymcp_jw_ulong(&r->w, ttl_ms);
    tinymcp_jw_key(&r->w, "cacheScope");
    tinymcp_jw_str(&r->w, "private");
}

static void error_begin(req_t *r, long code, const char *message)
{
    tinymcp_jw_t *w = &r->w;

    tinymcp_jw_reset(w, 0);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(w, &r->id);
    tinymcp_jw_raw(w, ",\"error\":{\"code\":");
    tinymcp_jw_long(w, code);
    tinymcp_jw_key(w, "message");
    tinymcp_jw_str(w, message);
}

static void error_end(req_t *r)
{
    tinymcp_jw_raw(&r->w, "}}");
    r->t->errors++;
    emit(r->t, &r->w);
}

static void error_reply(req_t *r, long code, const char *message)
{
    error_begin(r, code, message);
    error_end(r);
}

/* The code for a resource the host does not have, by era. */
static long not_found_code(const req_t *r)
{
    return r->modern ? -32602 : -32002;
}

static void versions_array(tinymcp_jw_t *w)
{
    size_t i;

    tinymcp_jw_raw(w, "[");
    for (i = 0; i < N_MODERN; i++) {
        tinymcp_jw_sep(w);
        tinymcp_jw_str(w, modern_versions[i]);
    }
    for (i = 0; i < N_LEGACY; i++) {
        tinymcp_jw_sep(w);
        tinymcp_jw_str(w, legacy_versions[i]);
    }
    tinymcp_jw_raw(w, "]");
}

static void capabilities(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;

    tinymcp_jw_key(w, "capabilities");
    tinymcp_jw_raw(w, "{");
    tinymcp_jw_key(w, "tools");
    tinymcp_jw_raw(w, "{}");
    if (h->resource_list) {
        tinymcp_jw_key(w, "resources");
        tinymcp_jw_raw(w, "{");
        tinymcp_jw_key(w, "subscribe");
        tinymcp_jw_raw(w, h->watch ? "true" : "false");
        tinymcp_jw_key(w, "listChanged");
        tinymcp_jw_raw(w, "true");
        tinymcp_jw_raw(w, "}");
    }
    tinymcp_jw_raw(w, "}");
}

/* Subscriptions */

static void slot_clear(tinymcp_t *t, unsigned i)
{
    tinymcp_sub_t *s = &t->subs[i];

    if (s->used && s->uri[0] && t->host->unwatch)
        t->host->unwatch(t->ctx, i);
    memset(s, 0, sizeof *s);
}

static unsigned slots_free(const tinymcp_t *t)
{
    unsigned i, n = 0;

    for (i = 0; i < t->max_subs; i++)
        if (!t->subs[i].used)
            n++;
    return n;
}

static int slot_take(tinymcp_t *t)
{
    unsigned i;

    for (i = 0; i < t->max_subs; i++) {
        if (!t->subs[i].used) {
            t->subs[i].used = 1;
            t->subs[i].seq = ++t->sub_seq;
            return (int)i;
        }
    }
    return -1;
}

/* The subscriptions/listen result: the stream has ended on the server's side. */
static void listen_closed(tinymcp_t *t, const char *id)
{
    tinymcp_jw_t w;

    tinymcp_jw_init(&w, t->out, t->out_cap);
    tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":");
    tinymcp_jw_raw(&w, id);
    tinymcp_jw_raw(&w, ",\"result\":{\"resultType\":\"complete\",\"_meta\":{\""
                       META_SUB "\":");
    tinymcp_jw_raw(&w, id);
    tinymcp_jw_raw(&w, "}}}");
    emit(t, &w);
}

/* End every slot a listen id holds; announce it when the server ends it. */
static void listen_end(tinymcp_t *t, const char *id, int announce)
{
    char copy[TINYMCP_ID_MAX];
    unsigned i, n = 0;

    strncpy(copy, id, sizeof copy - 1);
    copy[sizeof copy - 1] = 0;
    for (i = 0; i < t->max_subs; i++) {
        if (t->subs[i].used && !strcmp(t->subs[i].id, copy)) {
            slot_clear(t, i);
            n++;
        }
    }
    if (n && announce)
        listen_closed(t, copy);
}

/* Make room: the oldest listen, or the oldest handshake-era slot, goes. */
static void evict_oldest(tinymcp_t *t)
{
    unsigned i, oldest = 0, best = 0;
    int found = 0;

    for (i = 0; i < t->max_subs; i++) {
        if (t->subs[i].used && (!found || t->subs[i].seq < best)) {
            best = t->subs[i].seq;
            oldest = i;
            found = 1;
        }
    }
    if (!found)
        return;
    if (t->subs[oldest].id[0])
        listen_end(t, t->subs[oldest].id, 1);
    else
        slot_clear(t, oldest);
}

static void notify_updated(tinymcp_t *t, unsigned slot)
{
    tinymcp_sub_t *s = &t->subs[slot];
    tinymcp_jw_t w;

    tinymcp_jw_init(&w, t->out, t->out_cap);
    tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"notifications/resources/updated\",\"params\":{");
    if (s->id[0]) {
        tinymcp_jw_raw(&w, "\"_meta\":{\"" META_SUB "\":");
        tinymcp_jw_raw(&w, s->id);
        tinymcp_jw_raw(&w, "},");
    }
    tinymcp_jw_raw(&w, "\"uri\":");
    tinymcp_jw_str(&w, s->uri);
    tinymcp_jw_raw(&w, "}}");
    emit(t, &w);
}

static void notify_listchanged(tinymcp_t *t)
{
    tinymcp_jw_t w;
    unsigned i;

    for (i = 0; i < t->max_subs; i++) {
        if (!t->subs[i].used || !t->subs[i].listchanged)
            continue;
        tinymcp_jw_init(&w, t->out, t->out_cap);
        tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                           "\"notifications/resources/list_changed\","
                           "\"params\":{\"_meta\":{\"" META_SUB "\":");
        tinymcp_jw_raw(&w, t->subs[i].id);
        tinymcp_jw_raw(&w, "}}}");
        emit(t, &w);
    }
    if (t->era == TINYMCP_ERA_LEGACY) {
        tinymcp_jw_init(&w, t->out, t->out_cap);
        tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                           "\"notifications/resources/list_changed\"}");
        emit(t, &w);
    }
}

static void flush_pending(tinymcp_t *t)
{
    unsigned i;

    while (t->pending) {
        for (i = 0; i < t->max_subs; i++) {
            if (t->pending & (1u << i)) {
                t->pending &= ~(1u << i);
                if (t->subs[i].used)
                    notify_updated(t, i);
            }
        }
    }
    if (t->pending_lc) {
        t->pending_lc = 0;
        notify_listchanged(t);
    }
}

/* Parameters */

static int param(req_t *r, const char *key, tinymcp_jv_t *out)
{
    if (r->params.type != TINYMCP_J_OBJECT)
        return 0;
    return tinymcp_json_get(&r->params, key, out) == 1;
}

/* A string parameter decoded in place; NULL when absent or not a string. */
static char *param_str(req_t *r, const char *key)
{
    tinymcp_jv_t v;
    char *s;

    if (!param(r, key, &v) || v.type != TINYMCP_J_STRING)
        return NULL;
    s = (char *)v.at;
    if (tinymcp_json_decode(s, v.len) < 0)
        return NULL;
    return s;
}

/* The cursor parameter as a number; -1 when it is not one. */
static long param_cursor(req_t *r)
{
    tinymcp_jv_t v;
    long n = 0;
    size_t i;

    if (!param(r, "cursor", &v))
        return 0;
    if (v.type != TINYMCP_J_STRING)
        return -1;
    if (v.len == 0)
        return -1;
    for (i = 0; i < v.len; i++) {
        if (v.at[i] < '0' || v.at[i] > '9')
            return -1;
        n = n * 10 + (v.at[i] - '0');
    }
    return n;
}

static unsigned parse_unsigned(const char *s, size_t n)
{
    unsigned v = 0;
    size_t i;

    for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (unsigned)(s[i] - '0');
    return v;
}

/* Cut ?line=N&bytes=M off a URI, in place, and return the two numbers. */
static void split_query(char *uri, unsigned *line, unsigned *bytes)
{
    char *q = strchr(uri, '?'), *p;

    *line = 0;
    *bytes = 0;
    if (!q)
        return;
    *q = 0;
    for (p = q + 1; *p;) {
        char *end = strchr(p, '&');
        size_t n = end ? (size_t)(end - p) : strlen(p);

        if (n > 5 && !memcmp(p, "line=", 5))
            *line = parse_unsigned(p + 5, n - 5);
        else if (n > 6 && !memcmp(p, "bytes=", 6))
            *bytes = parse_unsigned(p + 6, n - 6);
        p += n + (end ? 1 : 0);
    }
}

/* Methods */

static void do_discover(req_t *r)
{
    tinymcp_jw_t *w = &r->w;

    begin_result(r);
    tinymcp_jw_key(w, "supportedVersions");
    versions_array(w);
    capabilities(r);
    if (r->t->host->instructions) {
        tinymcp_jw_key(w, "instructions");
        tinymcp_jw_str(w, r->t->host->instructions);
    }
    cacheable(r, TTL_STATIC);
    end_result(r);
}

static void do_initialize(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    tinymcp_jv_t v;
    const char *version = legacy_versions[0];
    size_t i;

    if (param(r, "protocolVersion", &v) && v.type == TINYMCP_J_STRING) {
        for (i = 0; i < N_LEGACY; i++)
            if (tinymcp_json_is(&v, legacy_versions[i]))
                version = legacy_versions[i];
    }
    r->t->era = TINYMCP_ERA_LEGACY;
    r->t->legacy_version = version;
    begin_result(r);
    tinymcp_jw_key(w, "protocolVersion");
    tinymcp_jw_str(w, version);
    capabilities(r);
    tinymcp_jw_key(w, "serverInfo");
    server_info(w, r->t->host);
    if (r->t->host->instructions) {
        tinymcp_jw_key(w, "instructions");
        tinymcp_jw_str(w, r->t->host->instructions);
    }
    end_result(r);
}

static void do_ping(req_t *r)
{
    begin_result(r);
    end_result(r);
}

static void do_tools_list(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;
    long cursor = param_cursor(r);
    size_t limit = r->t->out_cap - TAIL_RESERVE;
    unsigned i, next = 0;
    int more = 0;

    if (cursor < 0 || (unsigned long)cursor > h->ntools) {
        error_reply(r, -32602, "unknown cursor");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "tools");
    tinymcp_jw_raw(w, "[");
    for (i = (unsigned)cursor; i < h->ntools; i++) {
        const tinymcp_tool_t *tool = &h->tools[i];
        size_t mark = w->len;

        tinymcp_jw_sep(w);
        tinymcp_jw_raw(w, "{");
        tinymcp_jw_key(w, "name");
        tinymcp_jw_str(w, tool->name);
        if (tool->description) {
            tinymcp_jw_key(w, "description");
            tinymcp_jw_str(w, tool->description);
        }
        tinymcp_jw_key(w, "inputSchema");
        tinymcp_jw_raw(w, tool->input_schema);
        tinymcp_jw_raw(w, "}");
        if (w->full || w->len > limit) {
            tinymcp_jw_reset(w, mark);
            next = i;
            more = 1;
            break;
        }
    }
    if (more && next == (unsigned)cursor) {
        error_reply(r, -32603, "a tool entry does not fit the out buffer");
        return;
    }
    tinymcp_jw_raw(w, "]");
    if (more) {
        tinymcp_jw_key(w, "nextCursor");
        tinymcp_jw_raw(w, "\"");
        tinymcp_jw_ulong(w, next);
        tinymcp_jw_raw(w, "\"");
    }
    cacheable(r, TTL_STATIC);
    end_result(r);
}

static void do_templates_list(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;
    long cursor = param_cursor(r);
    size_t limit = r->t->out_cap - TAIL_RESERVE;
    unsigned i, next = 0;
    int more = 0;

    if (cursor < 0 || (unsigned long)cursor > h->ntemplates) {
        error_reply(r, -32602, "unknown cursor");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "resourceTemplates");
    tinymcp_jw_raw(w, "[");
    for (i = (unsigned)cursor; i < h->ntemplates; i++) {
        const tinymcp_template_t *tp = &h->templates[i];
        size_t mark = w->len;

        tinymcp_jw_sep(w);
        tinymcp_jw_raw(w, "{");
        tinymcp_jw_key(w, "uriTemplate");
        tinymcp_jw_str(w, tp->uri_template);
        tinymcp_jw_key(w, "name");
        tinymcp_jw_str(w, tp->name);
        if (tp->description) {
            tinymcp_jw_key(w, "description");
            tinymcp_jw_str(w, tp->description);
        }
        tinymcp_jw_key(w, "mimeType");
        tinymcp_jw_str(w, "text/plain");
        tinymcp_jw_raw(w, "}");
        if (w->full || w->len > limit) {
            tinymcp_jw_reset(w, mark);
            next = i;
            more = 1;
            break;
        }
    }
    if (more && next == (unsigned)cursor) {
        error_reply(r, -32603, "a template entry does not fit the out buffer");
        return;
    }
    tinymcp_jw_raw(w, "]");
    if (more) {
        tinymcp_jw_key(w, "nextCursor");
        tinymcp_jw_raw(w, "\"");
        tinymcp_jw_ulong(w, next);
        tinymcp_jw_raw(w, "\"");
    }
    cacheable(r, TTL_STATIC);
    end_result(r);
}

int tinymcp_list_add(tinymcp_list_t *list, const char *uri, const char *name,
                     const char *description)
{
    tinymcp_jw_t *w = list->w;
    size_t mark = w->len;

    tinymcp_jw_sep(w);
    tinymcp_jw_raw(w, "{");
    tinymcp_jw_key(w, "uri");
    tinymcp_jw_str(w, uri);
    tinymcp_jw_key(w, "name");
    tinymcp_jw_str(w, name);
    if (description) {
        tinymcp_jw_key(w, "description");
        tinymcp_jw_str(w, description);
    }
    tinymcp_jw_key(w, "mimeType");
    tinymcp_jw_str(w, "text/plain");
    tinymcp_jw_raw(w, "}");
    if (w->full || w->len > list->limit) {
        tinymcp_jw_reset(w, mark);
        return TINYMCP_EFULL;
    }
    list->count++;
    return 0;
}

static void do_resources_list(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;
    long cursor = param_cursor(r);
    tinymcp_list_t list;
    int next = 0;

    if (cursor < 0) {
        error_reply(r, -32602, "unknown cursor");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "resources");
    tinymcp_jw_raw(w, "[");
    list.w = w;
    list.limit = r->t->out_cap - TAIL_RESERVE;
    list.count = 0;
    if (h->resource_list)
        next = h->resource_list(r->t->ctx, (unsigned)cursor, &list);
    if (next == TINYMCP_EFULL || (next > 0 && list.count == 0)) {
        error_reply(r, -32603, "a resource entry does not fit the out buffer");
        return;
    }
    if (next < 0) {
        error_reply(r, -32603, "the host could not list");
        return;
    }
    tinymcp_jw_raw(w, "]");
    if (next > 0) {
        tinymcp_jw_key(w, "nextCursor");
        tinymcp_jw_raw(w, "\"");
        tinymcp_jw_ulong(w, (unsigned long)next);
        tinymcp_jw_raw(w, "\"");
    }
    cacheable(r, TTL_LIVE);
    end_result(r);
}

/* A host code into the reply; 1 when it was an error and the reply is sent. */
static int host_failed(req_t *r, int rc, const char *what)
{
    switch (rc) {
    case TINYMCP_ENOENT:
        error_reply(r, not_found_code(r), "no such resource");
        return 1;
    case TINYMCP_EEFFECT:
        error_reply(r, -32602, "reading it would act; use the read tool");
        return 1;
    case TINYMCP_EINVAL:
        error_reply(r, -32602, "invalid arguments");
        return 1;
    default:
        break;
    }
    if (rc <= -32000) {
        error_reply(r, rc, what);
        return 1;
    }
    if (rc < 0) {
        error_reply(r, -32603, what);
        return 1;
    }
    return 0;
}

static void do_resources_read(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;
    char *uri = param_str(r, "uri");
    tinymcp_sink_t sink;
    unsigned line, bytes;
    int rc;

    if (!uri) {
        error_reply(r, -32602, "uri missing");
        return;
    }
    if (!h->resource_read) {
        error_reply(r, not_found_code(r), "no such resource");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "contents");
    tinymcp_jw_raw(w, "[{\"uri\":");
    tinymcp_jw_str(w, uri);
    tinymcp_jw_raw(w, ",\"mimeType\":\"text/plain\",\"text\":\"");
    tinymcp_sink_init(&sink, w, r->t->out_cap - TAIL_RESERVE);
    split_query(uri, &line, &bytes);
    rc = h->resource_read(r->t->ctx, uri, line, bytes, &sink);
    if (host_failed(r, rc, "the host could not read"))
        return;
    if (sink.cut)
        tinymcp_jw_raw(w, "\\n[cut]");
    tinymcp_jw_raw(w, "\"}]");
    cacheable(r, TTL_LIVE);
    end_result(r);
}

/* Collect the arguments object, then decode its strings in place. */
static int collect_args(req_t *r, tinymcp_args_t *args)
{
    tinymcp_jv_t obj, k, v;
    tinymcp_jv_t keys[TINYMCP_ARGS_MAX], vals[TINYMCP_ARGS_MAX];
    size_t pos = 0;
    unsigned i;
    int rc;

    args->n = 0;
    if (!param(r, "arguments", &obj))
        return 0;
    if (obj.type != TINYMCP_J_OBJECT)
        return -1;
    while (args->n < TINYMCP_ARGS_MAX &&
           (rc = tinymcp_json_next(&obj, &pos, &k, &v)) == 1) {
        keys[args->n] = k;
        vals[args->n] = v;
        args->n++;
    }
    for (i = 0; i < args->n; i++) {
        tinymcp_arg_t *a = &args->a[i];
        char *s = (char *)keys[i].at;

        if (tinymcp_json_decode(s, keys[i].len) < 0)
            return -1;
        a->key = s;
        a->type = vals[i].type;
        a->raw = vals[i].at;
        a->raw_len = vals[i].len;
        a->str = NULL;
        a->num = 0;
        if (vals[i].type == TINYMCP_J_STRING) {
            s = (char *)vals[i].at;
            if (tinymcp_json_decode(s, vals[i].len) < 0)
                return -1;
            a->str = s;
        } else if (vals[i].type == TINYMCP_J_NUMBER) {
            a->num = tinymcp_json_long(&vals[i]);
        }
    }
    return 0;
}

static void do_tools_call(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;
    tinymcp_args_t args;
    tinymcp_sink_t sink;
    const char *name;
    unsigned i;
    int rc, found = 0;

    /* The arguments are decoded before the name: both sit in params. */
    if (collect_args(r, &args) < 0) {
        error_reply(r, -32602, "arguments must be an object of strings and numbers");
        return;
    }
    name = param_str(r, "name");
    if (!name) {
        error_reply(r, -32602, "name missing");
        return;
    }
    for (i = 0; i < h->ntools; i++)
        if (!strcmp(h->tools[i].name, name))
            found = 1;
    if (!found || !h->tool_call) {
        error_reply(r, -32602, "unknown tool");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "content");
    tinymcp_jw_raw(w, "[{\"type\":\"text\",\"text\":\"");
    tinymcp_sink_init(&sink, w, r->t->out_cap - TAIL_RESERVE);
    rc = h->tool_call(r->t->ctx, name, &args, &sink);
    if (rc != TINYMCP_TOOL_ERROR && host_failed(r, rc, "the tool failed"))
        return;
    if (sink.cut)
        tinymcp_jw_raw(w, "\\n[cut]");
    tinymcp_jw_raw(w, "\"}]");
    if (rc == TINYMCP_TOOL_ERROR || sink.cut) {
        tinymcp_jw_key(w, "isError");
        tinymcp_jw_raw(w, "true");
    }
    end_result(r);
}

/* The listen id as it was sent, into dst; 0 when it fits. */
static int id_text(const tinymcp_jv_t *id, char *dst, size_t cap)
{
    size_t n = id->len + (id->type == TINYMCP_J_STRING ? 2 : 0);

    if (n + 1 > cap || id->type == TINYMCP_J_NONE)
        return -1;
    if (id->type == TINYMCP_J_STRING) {
        dst[0] = '"';
        memcpy(dst + 1, id->at, id->len);
        dst[n - 1] = '"';
    } else {
        memcpy(dst, id->at, id->len);
    }
    dst[n] = 0;
    return 0;
}

static void do_listen(req_t *r)
{
    tinymcp_t *t = r->t;
    tinymcp_jw_t *w = &r->w;
    tinymcp_jv_t filter, v, item;
    char id[TINYMCP_ID_MAX];
    unsigned want_lc = 0, nuris = 0, needed, first = 0, i;
    int have_first = 0, taken;
    size_t pos = 0;

    if (id_text(&r->id, id, sizeof id) < 0) {
        error_reply(r, -32602, "id too long for a listen");
        return;
    }
    if (!param(r, "notifications", &filter) ||
        filter.type != TINYMCP_J_OBJECT) {
        error_reply(r, -32602, "notifications missing");
        return;
    }
    if (tinymcp_json_get(&filter, "resourcesListChanged", &v) == 1 &&
        v.type == TINYMCP_J_TRUE)
        want_lc = 1;
    if (tinymcp_json_get(&filter, "resourceSubscriptions", &v) == 1 &&
        v.type == TINYMCP_J_ARRAY && t->host->watch) {
        while (tinymcp_json_item(&v, &pos, &item) == 1)
            nuris++;
    } else {
        v.type = TINYMCP_J_NONE;
    }
    /* A listen under an id already open replaces it. */
    listen_end(t, id, 0);
    needed = nuris ? nuris : (want_lc ? 1 : 0);
    if (needed > t->max_subs)
        needed = t->max_subs;
    while (needed && slots_free(t) < needed)
        evict_oldest(t);
    pos = 0;
    while (v.type == TINYMCP_J_ARRAY &&
           tinymcp_json_item(&v, &pos, &item) == 1) {
        tinymcp_sub_t *s;

        if (item.type != TINYMCP_J_STRING || item.len >= TINYMCP_URI_MAX ||
            slots_free(t) == 0)
            continue;
        taken = slot_take(t);
        s = &t->subs[taken];
        memcpy(s->uri, item.at, item.len);
        s->uri[item.len] = 0;
        if (tinymcp_json_decode(s->uri, item.len) < 0 ||
            t->host->watch(t->ctx, s->uri, (unsigned)taken) != 0) {
            memset(s, 0, sizeof *s);
            continue;
        }
        strcpy(s->id, id);
        if (!have_first) {
            first = (unsigned)taken;
            have_first = 1;
        }
    }
    if (want_lc) {
        if (!have_first) {
            taken = slot_take(t);
            if (taken >= 0) {
                strcpy(t->subs[taken].id, id);
                first = (unsigned)taken;
                have_first = 1;
            } else {
                want_lc = 0;
            }
        }
        if (have_first)
            t->subs[first].listchanged = 1;
    }
    /* The acknowledgment is the first message on the stream. */
    tinymcp_jw_reset(w, 0);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"method\":"
                      "\"notifications/subscriptions/acknowledged\","
                      "\"params\":{\"_meta\":{\"" META_SUB "\":");
    tinymcp_jw_raw(w, id);
    tinymcp_jw_raw(w, "},\"notifications\":{");
    if (want_lc) {
        tinymcp_jw_key(w, "resourcesListChanged");
        tinymcp_jw_raw(w, "true");
    }
    if (nuris) {
        tinymcp_jw_key(w, "resourceSubscriptions");
        tinymcp_jw_raw(w, "[");
        for (i = 0; i < t->max_subs; i++) {
            if (t->subs[i].used && t->subs[i].uri[0] &&
                !strcmp(t->subs[i].id, id)) {
                tinymcp_jw_sep(w);
                tinymcp_jw_str(w, t->subs[i].uri);
            }
        }
        tinymcp_jw_raw(w, "]");
    }
    tinymcp_jw_raw(w, "}}}");
    emit(t, w);
}

static int legacy_slot_of(tinymcp_t *t, const char *uri)
{
    unsigned i;

    for (i = 0; i < t->max_subs; i++)
        if (t->subs[i].used && !t->subs[i].id[0] &&
            !strcmp(t->subs[i].uri, uri))
            return (int)i;
    return -1;
}

static void do_subscribe(req_t *r)
{
    tinymcp_t *t = r->t;
    char *uri = param_str(r, "uri");
    int slot;

    if (!uri) {
        error_reply(r, -32602, "uri missing");
        return;
    }
    if (strlen(uri) >= TINYMCP_URI_MAX) {
        error_reply(r, -32602, "uri too long");
        return;
    }
    if (legacy_slot_of(t, uri) < 0) {
        if (slots_free(t) == 0)
            evict_oldest(t);
        slot = slot_take(t);
        if (slot < 0) {
            error_reply(r, -32603, "no subscription slot");
            return;
        }
        strcpy(t->subs[slot].uri, uri);
        if (t->host->watch(t->ctx, uri, (unsigned)slot) != 0) {
            memset(&t->subs[slot], 0, sizeof t->subs[slot]);
            error_reply(r, not_found_code(r), "no such resource");
            return;
        }
    }
    begin_result(r);
    end_result(r);
}

static void do_unsubscribe(req_t *r)
{
    char *uri = param_str(r, "uri");
    int slot;

    if (!uri) {
        error_reply(r, -32602, "uri missing");
        return;
    }
    slot = legacy_slot_of(r->t, uri);
    if (slot >= 0)
        slot_clear(r->t, (unsigned)slot);
    begin_result(r);
    end_result(r);
}

static void do_notification(req_t *r)
{
    tinymcp_jv_t v;
    char id[TINYMCP_ID_MAX];

    if (!strcmp(r->method, "notifications/cancelled") &&
        param(r, "requestId", &v) && id_text(&v, id, sizeof id) == 0)
        listen_end(r->t, id, 0);
}

static int version_supported(const tinymcp_jv_t *v)
{
    size_t i;

    for (i = 0; i < N_MODERN; i++)
        if (tinymcp_json_is(v, modern_versions[i]))
            return 1;
    return 0;
}

static void unsupported_version(req_t *r, const tinymcp_jv_t *pv)
{
    tinymcp_jw_t *w = &r->w;

    error_begin(r, -32022, "unsupported protocol version");
    tinymcp_jw_key(w, "data");
    tinymcp_jw_raw(w, "{\"supported\":");
    versions_array(w);
    tinymcp_jw_key(w, "requested");
    tinymcp_jw_strn(w, pv->at, pv->len);
    tinymcp_jw_raw(w, "}");
    error_end(r);
}

static void dispatch(req_t *r)
{
    const char *m = r->method;
    int legacy_era = r->t->era == TINYMCP_ERA_LEGACY;

    if (!r->modern && !legacy_era && strcmp(m, "initialize") &&
        strcmp(m, "ping")) {
        error_reply(r, -32602, "_meta." META_PV " missing: send "
                               "server/discover, or initialize first");
        return;
    }
    if (r->modern && r->t->era == TINYMCP_ERA_NONE)
        r->t->era = TINYMCP_ERA_MODERN;
    if (!strcmp(m, "server/discover") && r->modern)
        do_discover(r);
    else if (!strcmp(m, "initialize") && !r->modern)
        do_initialize(r);
    else if (!strcmp(m, "ping"))
        do_ping(r);
    else if (!strcmp(m, "tools/list"))
        do_tools_list(r);
    else if (!strcmp(m, "tools/call"))
        do_tools_call(r);
    else if (!strcmp(m, "resources/list"))
        do_resources_list(r);
    else if (!strcmp(m, "resources/templates/list"))
        do_templates_list(r);
    else if (!strcmp(m, "resources/read"))
        do_resources_read(r);
    else if (!strcmp(m, "subscriptions/listen") && r->modern)
        do_listen(r);
    else if (!strcmp(m, "resources/subscribe") && !r->modern &&
             r->t->host->watch)
        do_subscribe(r);
    else if (!strcmp(m, "resources/unsubscribe") && !r->modern &&
             r->t->host->watch)
        do_unsubscribe(r);
    else
        error_reply(r, -32601, "unknown method");
}

static void process(tinymcp_t *t, char *msg, size_t len)
{
    req_t r;
    tinymcp_jv_t root, v, meta, pv;
    size_t k;
    int rc;

    memset(&r, 0, sizeof r);
    r.t = t;
    tinymcp_jw_init(&r.w, t->out, t->out_cap);
    t->requests++;
    t->busy = 1;
    k = tinymcp_json_value(msg, len, &root);
    if (!k || root.type != TINYMCP_J_OBJECT) {
        error_reply(&r, -32700, "parse error");
        goto out;
    }
    rc = tinymcp_json_get(&root, "id", &r.id);
    if (rc < 0) {
        error_reply(&r, -32700, "parse error");
        goto out;
    }
    if (rc == 0)
        r.id.type = TINYMCP_J_NONE;
    if (tinymcp_json_get(&root, "method", &v) != 1 ||
        v.type != TINYMCP_J_STRING) {
        /* A response to a request this server never sends. */
        if (tinymcp_json_get(&root, "result", &v) == 1 ||
            tinymcp_json_get(&root, "error", &v) == 1)
            goto out;
        error_reply(&r, -32600, "method missing");
        goto out;
    }
    rc = tinymcp_json_get(&root, "params", &r.params);
    if (rc == 0)
        r.params.type = TINYMCP_J_NONE;
    if (rc < 0 || (rc == 1 && r.params.type != TINYMCP_J_OBJECT)) {
        error_reply(&r, -32602, "params must be an object");
        goto out;
    }
    /* Nothing above is scanned again once the method is decoded. */
    if (tinymcp_json_decode((char *)v.at, v.len) < 0) {
        error_reply(&r, -32600, "method is not a string");
        goto out;
    }
    r.method = v.at;
    if (r.id.type == TINYMCP_J_NONE) {
        do_notification(&r);
        goto out;
    }
    if (r.id.type == TINYMCP_J_NULL) {
        r.id.type = TINYMCP_J_NONE;
        error_reply(&r, -32600, "id must not be null");
        goto out;
    }
    if (param(&r, "_meta", &meta) && meta.type == TINYMCP_J_OBJECT &&
        tinymcp_json_get(&meta, META_PV, &pv) == 1) {
        if (pv.type != TINYMCP_J_STRING) {
            error_reply(&r, -32602, "_meta." META_PV " must be a string");
            goto out;
        }
        if (!version_supported(&pv)) {
            unsupported_version(&r, &pv);
            goto out;
        }
        if (tinymcp_json_get(&meta, META_CAPS, &v) != 1 ||
            v.type != TINYMCP_J_OBJECT) {
            error_reply(&r, -32602, "_meta." META_CAPS " missing");
            goto out;
        }
        r.modern = 1;
    }
    dispatch(&r);
out:
    t->busy = 0;
    flush_pending(t);
}

/* The API */

void tinymcp_init(tinymcp_t *t, const tinymcp_host_t *host, void *ctx,
                  char *in, size_t in_cap, char *out, size_t out_cap,
                  unsigned max_subs)
{
    memset(t, 0, sizeof *t);
    t->host = host;
    t->ctx = ctx;
    t->in = in;
    t->in_cap = in_cap;
    t->out = out;
    t->out_cap = out_cap;
    t->max_subs = max_subs > TINYMCP_SUBS_MAX ? TINYMCP_SUBS_MAX : max_subs;
    if (in_cap)
        in[0] = 0;
    if (out_cap)
        out[0] = 0;
}

static void too_large(tinymcp_t *t)
{
    tinymcp_jw_t w;

    tinymcp_jw_init(&w, t->out, t->out_cap);
    tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":"
                       "-32700,\"message\":\"message larger than ");
    tinymcp_jw_ulong(&w, (unsigned long)(t->in_cap - 1));
    tinymcp_jw_raw(&w, " bytes\"}}");
    t->errors++;
    t->requests++;
    emit(t, &w);
}

static int blank(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r')
            return 0;
    return 1;
}

int tinymcp_feed(tinymcp_t *t, const void *bytes, size_t len)
{
    const char *p = bytes;
    size_t i;
    int handled = 0;

    for (i = 0; i < len; i++) {
        char c = p[i];

        if (c == '\n') {
            if (t->in_drop) {
                t->in_drop = 0;
                too_large(t);
            } else if (!blank(t->in, t->in_len)) {
                t->in[t->in_len] = 0;
                process(t, t->in, t->in_len);
                handled++;
            }
            t->in_len = 0;
            continue;
        }
        if (t->in_drop)
            continue;
        if (t->in_len + 1 >= t->in_cap) {
            t->in_drop = 1;
            t->in_len = 0;
            continue;
        }
        t->in[t->in_len++] = c;
    }
    return handled;
}

int tinymcp_message(tinymcp_t *t, const void *msg, size_t len)
{
    const char *p = msg;

    while (len && (p[len - 1] == '\n' || p[len - 1] == '\r'))
        len--;
    if (len + 1 > t->in_cap) {
        too_large(t);
        return -1;
    }
    memcpy(t->in, p, len);
    t->in_len = 0;
    t->in_drop = 0;
    tinymcp_handle(t, t->in, len);
    return 0;
}

void tinymcp_handle(tinymcp_t *t, char *msg, size_t len)
{
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
        len--;
    msg[len] = 0;
    if (!blank(msg, len))
        process(t, msg, len);
}

void tinymcp_updated(tinymcp_t *t, unsigned slot)
{
    if (slot >= t->max_subs || !t->subs[slot].used || !t->subs[slot].uri[0])
        return;
    if (t->busy) {
        t->pending |= 1u << slot;
        return;
    }
    notify_updated(t, slot);
}

void tinymcp_listchanged(tinymcp_t *t)
{
    if (t->busy) {
        t->pending_lc = 1;
        return;
    }
    notify_listchanged(t);
}

void tinymcp_closed(tinymcp_t *t)
{
    unsigned i;

    for (i = 0; i < t->max_subs; i++)
        if (t->subs[i].used)
            slot_clear(t, i);
    t->era = TINYMCP_ERA_NONE;
    t->legacy_version = NULL;
    t->in_len = 0;
    t->in_drop = 0;
    t->pending = 0;
    t->pending_lc = 0;
}

const char *tinymcp_era_name(const tinymcp_t *t)
{
    switch (t->era) {
    case TINYMCP_ERA_MODERN:
        return "modern";
    case TINYMCP_ERA_LEGACY:
        return "legacy";
    default:
        return "none";
    }
}

const char *tinymcp_arg_str(const tinymcp_args_t *args, const char *key)
{
    unsigned i;

    for (i = 0; i < args->n; i++)
        if (!strcmp(args->a[i].key, key))
            return args->a[i].str;
    return NULL;
}

long tinymcp_arg_int(const tinymcp_args_t *args, const char *key, long dflt)
{
    unsigned i;

    for (i = 0; i < args->n; i++)
        if (!strcmp(args->a[i].key, key))
            return args->a[i].type == TINYMCP_J_NUMBER ? args->a[i].num : dflt;
    return dflt;
}

int tinymcp_arg_bool(const tinymcp_args_t *args, const char *key, int dflt)
{
    unsigned i;

    for (i = 0; i < args->n; i++) {
        if (!strcmp(args->a[i].key, key)) {
            if (args->a[i].type == TINYMCP_J_TRUE)
                return 1;
            if (args->a[i].type == TINYMCP_J_FALSE)
                return 0;
            return dflt;
        }
    }
    return dflt;
}
