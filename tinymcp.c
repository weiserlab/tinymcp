/*
 * tinymcp v0.7
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp.c - one message in, one reply out, in either era; content past
 * the out buffer streams through send_part, and a tool call the host keeps
 * is answered later.  Tools, resources and prompts are the host's table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tinymcp.h"

#include <string.h>

#define META_PV   "io.modelcontextprotocol/protocolVersion"
#define META_CAPS "io.modelcontextprotocol/clientCapabilities"
#define META_SUB  "io.modelcontextprotocol/subscriptionId"
#define META_SRV  "io.modelcontextprotocol/serverInfo"
#define META_LIB  "org.tinymcp/library"
#define META_CLI  "io.modelcontextprotocol/clientInfo"

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
    /* A message begun in pieces ends in one, whatever happened to it. */
    if (w->sent) {
        if (t->host->send_part(t->ctx, w->buf, w->len, 1) != 0 || w->full)
            t->dropped++;
        return;
    }
    if (w->full || t->host->send(t->ctx, w->buf, w->len) != 0)
        t->dropped++;
}

static int send_part(void *ctx, const char *bytes, size_t n, int last)
{
    tinymcp_t *t = ctx;

    return t->host->send_part(t->ctx, bytes, n, last);
}

/* Content past the out buffer goes out in pieces, where the host can. */
static void stream_content(tinymcp_t *t, tinymcp_jw_t *w)
{
    if (t->host->send_part)
        tinymcp_jw_stream(w, send_part, t);
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
    if (h->title) {
        tinymcp_jw_key(w, "title");
        tinymcp_jw_str(w, h->title);
    }
    tinymcp_jw_key(w, "version");
    tinymcp_jw_str(w, h->version);
    tinymcp_jw_raw(w, "}");
}

/* The library's name and version, as a _meta member. */
static void library_meta(tinymcp_jw_t *w)
{
    tinymcp_jw_key(w, META_LIB);
    tinymcp_jw_str(w, "tinymcp " TINYMCP_VERSION);
}

/* A result's first members, by revision; library adds the library's _meta
 * member. */
static void result_head(tinymcp_t *t, tinymcp_jw_t *w, int modern,
                        int library)
{
    if (modern) {
        tinymcp_jw_raw(w, "\"resultType\":\"complete\",\"_meta\":{\"" META_SRV
                          "\":");
        server_info(w, t->host);
        if (library)
            library_meta(w);
        tinymcp_jw_raw(w, "}");
    } else if (library) {
        tinymcp_jw_key(w, "_meta");
        tinymcp_jw_raw(w, "{");
        library_meta(w);
        tinymcp_jw_raw(w, "}");
    }
}

/* The result's opening. */
static void begin_result_with(req_t *r, int library)
{
    tinymcp_jw_t *w = &r->w;

    tinymcp_jw_reset(w, 0);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(w, &r->id);
    tinymcp_jw_raw(w, ",\"result\":{");
    result_head(r->t, w, r->modern, library);
}

static void begin_result(req_t *r)
{
    begin_result_with(r, 0);
}

static void error_reply(req_t *r, long code, const char *message);

static void end_result(req_t *r)
{
    tinymcp_jw_raw(&r->w, "}}");
    if (r->w.full && !r->w.sent) {
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
    if (h->prompt_list) {
        tinymcp_jw_key(w, "prompts");
        tinymcp_jw_raw(w, "{\"listChanged\":true}");
    }
    if (h->complete) {
        tinymcp_jw_key(w, "completions");
        tinymcp_jw_raw(w, "{}");
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

/* A list_changed notice of one kind (TINYMCP_LC_*) to every listen that
 * asked for it, and to the stream when it is in the handshake era. */
static void notify_listchanged(tinymcp_t *t, unsigned kind)
{
    const char *method = kind == TINYMCP_LC_PROMPTS
                             ? "notifications/prompts/list_changed"
                             : "notifications/resources/list_changed";
    tinymcp_jw_t w;
    unsigned i;

    for (i = 0; i < t->max_subs; i++) {
        if (!t->subs[i].used || !(t->subs[i].listchanged & kind))
            continue;
        tinymcp_jw_init(&w, t->out, t->out_cap);
        tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"");
        tinymcp_jw_raw(&w, method);
        tinymcp_jw_raw(&w, "\",\"params\":{\"_meta\":{\"" META_SUB "\":");
        tinymcp_jw_raw(&w, t->subs[i].id);
        tinymcp_jw_raw(&w, "}}}");
        emit(t, &w);
    }
    if (t->era == TINYMCP_ERA_LEGACY) {
        tinymcp_jw_init(&w, t->out, t->out_cap);
        tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":\"");
        tinymcp_jw_raw(&w, method);
        tinymcp_jw_raw(&w, "\"}");
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
        unsigned kinds = t->pending_lc;

        t->pending_lc = 0;
        if (kinds & TINYMCP_LC_RESOURCES)
            notify_listchanged(t, TINYMCP_LC_RESOURCES);
        if (kinds & TINYMCP_LC_PROMPTS)
            notify_listchanged(t, TINYMCP_LC_PROMPTS);
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

    begin_result_with(r, 1);
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

/* The name member of a clientInfo object into the instance, decoded there:
 * the message itself is left as it arrived. */
static void keep_client(tinymcp_t *t, const tinymcp_jv_t *info)
{
    tinymcp_jv_t name;
    size_t n;

    t->client[0] = 0;
    if (info->type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(info, "name", &name) != 1 ||
        name.type != TINYMCP_J_STRING)
        return;
    n = name.len < TINYMCP_CLIENT_MAX - 1 ? name.len : TINYMCP_CLIENT_MAX - 1;
    /* A cut inside an escape does not decode; a shorter cut does. */
    for (; n > 0; n--) {
        memcpy(t->client, name.at, n);
        if (tinymcp_json_decode(t->client, n) >= 0)
            return;
    }
    t->client[0] = 0;
}

static void do_initialize(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    tinymcp_jv_t v;
    const char *version = legacy_versions[0];
    size_t i;

    if (param(r, "clientInfo", &v))
        keep_client(r->t, &v);
    if (param(r, "protocolVersion", &v) && v.type == TINYMCP_J_STRING) {
        for (i = 0; i < N_LEGACY; i++)
            if (tinymcp_json_is(&v, legacy_versions[i]))
                version = legacy_versions[i];
    }
    r->t->era = TINYMCP_ERA_LEGACY;
    r->t->legacy_version = version;
    begin_result_with(r, 1);
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
        if (tool->output_schema) {
            tinymcp_jw_key(w, "outputSchema");
            tinymcp_jw_raw(w, tool->output_schema);
        }
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

/* Keep the entry written since mark when it fits the page; else drop it. */
static int list_fit(tinymcp_list_t *list, size_t mark)
{
    tinymcp_jw_t *w = list->w;

    if (w->full || w->len > list->limit) {
        tinymcp_jw_reset(w, mark);
        return TINYMCP_EFULL;
    }
    list->count++;
    return 0;
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
    return list_fit(list, mark);
}

int tinymcp_prompt_add(tinymcp_list_t *list, const char *name,
                       const char *description,
                       const tinymcp_prompt_arg_t *args, unsigned nargs)
{
    tinymcp_jw_t *w = list->w;
    size_t mark = w->len;
    unsigned i;

    tinymcp_jw_sep(w);
    tinymcp_jw_raw(w, "{");
    tinymcp_jw_key(w, "name");
    tinymcp_jw_str(w, name);
    if (description) {
        tinymcp_jw_key(w, "description");
        tinymcp_jw_str(w, description);
    }
    if (nargs) {
        tinymcp_jw_key(w, "arguments");
        tinymcp_jw_raw(w, "[");
        for (i = 0; i < nargs; i++) {
            tinymcp_jw_sep(w);
            tinymcp_jw_raw(w, "{");
            tinymcp_jw_key(w, "name");
            tinymcp_jw_str(w, args[i].name);
            if (args[i].description) {
                tinymcp_jw_key(w, "description");
                tinymcp_jw_str(w, args[i].description);
            }
            tinymcp_jw_key(w, "required");
            tinymcp_jw_raw(w, args[i].required ? "true" : "false");
            tinymcp_jw_raw(w, "}");
        }
        tinymcp_jw_raw(w, "]");
    }
    tinymcp_jw_raw(w, "}");
    return list_fit(list, mark);
}

/* A paged list from the host's lister fn under key: resources or prompts. */
static void do_host_list(req_t *r, const char *key,
                         int (*fn)(void *, unsigned, tinymcp_list_t *))
{
    tinymcp_jw_t *w = &r->w;
    long cursor = param_cursor(r);
    tinymcp_list_t list;
    int next = 0;

    if (cursor < 0) {
        error_reply(r, -32602, "unknown cursor");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, key);
    tinymcp_jw_raw(w, "[");
    list.w = w;
    list.limit = r->t->out_cap - TAIL_RESERVE;
    list.count = 0;
    if (fn)
        next = fn(r->t->ctx, (unsigned)cursor, &list);
    if (next == TINYMCP_EFULL || (next > 0 && list.count == 0)) {
        error_reply(r, -32603, "an entry does not fit the out buffer");
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

static void do_resources_list(req_t *r)
{
    do_host_list(r, "resources", r->t->host->resource_list);
}

static void do_prompts_list(req_t *r)
{
    do_host_list(r, "prompts", r->t->host->prompt_list);
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

/* The InputRequiredResult for the elicitation the host described. */
static void input_required(req_t *r)
{
    tinymcp_t *t = r->t;
    tinymcp_jw_t *w = &r->w;

    if (!t->el_key || !t->el_message || !t->el_schema) {
        error_reply(r, -32603, "the host asked for input it did not describe");
        return;
    }
    tinymcp_jw_reset(w, 0);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    write_id(w, &r->id);
    tinymcp_jw_raw(w, ",\"result\":{\"resultType\":\"input_required\","
                      "\"_meta\":{\"" META_SRV "\":");
    server_info(w, t->host);
    tinymcp_jw_raw(w, "}");
    tinymcp_jw_key(w, "inputRequests");
    tinymcp_jw_raw(w, "{");
    tinymcp_jw_key(w, t->el_key);
    tinymcp_jw_raw(w, "{\"method\":\"elicitation/create\",\"params\":"
                      "{\"mode\":\"form\",\"message\":");
    tinymcp_jw_str(w, t->el_message);
    tinymcp_jw_raw(w, ",\"requestedSchema\":");
    tinymcp_jw_raw(w, t->el_schema);
    tinymcp_jw_raw(w, "}}}");
    if (t->el_state) {
        tinymcp_jw_key(w, "requestState");
        tinymcp_jw_str(w, t->el_state);
    }
    end_result(r);
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
    tinymcp_sink_init(&sink, w, r->t->out_cap - TAIL_RESERVE);
    tinymcp_sink_lazy(&sink, TINYMCP_SINK_RESOURCE);
    stream_content(r->t, w);
    split_query(uri, &line, &bytes);
    rc = h->resource_read(r->t->ctx, uri, line, bytes, &sink);
    /* Once a piece is out the reply is committed: it ends, short. */
    if (w->sent && rc < 0)
        r->t->errors++;
    else if (rc == TINYMCP_NEED_INPUT) {
        input_required(r);
        return;
    } else if (host_failed(r, rc, "the host could not read"))
        return;
    tinymcp_sink_end(&sink);
    if (sink.cut && sink.b64) {
        error_reply(r, -32603, "the resource does not fit the out buffer");
        return;
    }
    if (sink.cut)
        tinymcp_jw_raw(w, "\\n[cut]");
    tinymcp_jw_raw(w, "\"}]");
    cacheable(r, TTL_LIVE);
    end_result(r);
}

/*
 * Collect an arguments object, then decode its strings in place; obj absent
 * (TINYMCP_J_NONE) is no arguments.  Nothing that encloses obj may be
 * scanned afterwards.
 */
static int collect_object(const tinymcp_jv_t *obj, tinymcp_args_t *args)
{
    tinymcp_jv_t k, v;
    tinymcp_jv_t keys[TINYMCP_ARGS_MAX], vals[TINYMCP_ARGS_MAX];
    size_t pos = 0;
    unsigned i;
    int rc;

    args->n = 0;
    if (obj->type == TINYMCP_J_NONE)
        return 0;
    if (obj->type != TINYMCP_J_OBJECT)
        return -1;
    while (args->n < TINYMCP_ARGS_MAX &&
           (rc = tinymcp_json_next(obj, &pos, &k, &v)) == 1) {
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

/* A located string decoded in place; NULL when v is not a string. */
static char *decoded(const tinymcp_jv_t *v)
{
    char *s = (char *)v->at;

    if (v->type != TINYMCP_J_STRING || tinymcp_json_decode(s, v->len) < 0)
        return NULL;
    return s;
}

/*
 * The name and arguments params, both located before either is decoded:
 * decoding shrinks a string in place, so params is not scanned after it.
 */
static int name_and_args(req_t *r, const char **name, tinymcp_args_t *args)
{
    tinymcp_jv_t n, a;

    if (!param(r, "name", &n))
        n.type = TINYMCP_J_NONE;
    if (!param(r, "arguments", &a))
        a.type = TINYMCP_J_NONE;
    if (collect_object(&a, args) < 0) {
        error_reply(r, -32602, "arguments must be an object of strings and numbers");
        return -1;
    }
    *name = decoded(&n);
    if (!*name) {
        error_reply(r, -32602, "name missing");
        return -1;
    }
    return 0;
}

/*
 * The content array's end, isError and structuredContent; mark is where
 * the array's first item begins.  Binary content that was cut is replaced
 * by a text saying so.
 */
static void end_content(tinymcp_t *t, tinymcp_jw_t *w, tinymcp_sink_t *sink,
                        size_t mark, int is_error)
{
    tinymcp_sink_end(sink);
    if (sink->cut && sink->b64) {
        tinymcp_jw_reset(w, mark);
        tinymcp_jw_raw(w, "{\"type\":\"text\",\"text\":\"the content does "
                          "not fit the out buffer");
    } else if (sink->cut) {
        tinymcp_jw_raw(w, "\\n[cut]");
    }
    tinymcp_jw_raw(w, "\"}]");
    if (is_error || sink->cut) {
        tinymcp_jw_key(w, "isError");
        tinymcp_jw_raw(w, "true");
    }
    if (t->structured) {
        tinymcp_jw_key(w, "structuredContent");
        tinymcp_jw_raw(w, t->structured);
    }
}

static void do_tools_call(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    const tinymcp_host_t *h = r->t->host;
    tinymcp_args_t args;
    tinymcp_sink_t sink;
    const char *name;
    size_t mark;
    unsigned i;
    int rc, found = 0;

    if (name_and_args(r, &name, &args) < 0)
        return;
    for (i = 0; i < h->ntools; i++)
        if (!strcmp(h->tools[i].name, name))
            found = 1;
    if (!found || !h->tool_call) {
        error_reply(r, -32602, "unknown tool");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "content");
    tinymcp_jw_raw(w, "[");
    mark = w->len;
    tinymcp_sink_init(&sink, w, r->t->out_cap - TAIL_RESERVE);
    tinymcp_sink_lazy(&sink, TINYMCP_SINK_TOOL);
    stream_content(r->t, w);
    r->t->in_call = 1;
    rc = h->tool_call(r->t->ctx, name, &args, &sink);
    r->t->in_call = 0;
    if (rc == TINYMCP_LATER) {
        if (!r->t->deferred)
            error_reply(r, -32603, "the tool kept no call");
        return;
    }
    /* Kept and answered now after all: the call is not open. */
    if (r->t->deferred)
        memset(&r->t->later[r->t->deferred - 1], 0, sizeof r->t->later[0]);
    /* Once a piece is out the reply is committed: a failure is isError. */
    if (w->sent && rc != TINYMCP_OK) {
        if (rc != TINYMCP_TOOL_ERROR)
            r->t->errors++;
        rc = TINYMCP_TOOL_ERROR;
    }
    if (rc == TINYMCP_NEED_INPUT) {
        input_required(r);
        return;
    }
    if (rc != TINYMCP_TOOL_ERROR && host_failed(r, rc, "the tool failed"))
        return;
    end_content(r->t, w, &sink, mark, rc == TINYMCP_TOOL_ERROR);
    end_result(r);
}

static void do_prompts_get(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    tinymcp_args_t args;
    tinymcp_sink_t sink;
    const char *name;
    int rc;

    if (name_and_args(r, &name, &args) < 0)
        return;
    begin_result(r);
    tinymcp_jw_key(w, "messages");
    tinymcp_jw_raw(w, "[{\"role\":\"user\",\"content\":{\"type\":\"text\","
                      "\"text\":\"");
    tinymcp_sink_init(&sink, w, r->t->out_cap - TAIL_RESERVE);
    rc = r->t->host->prompt_get(r->t->ctx, name, &args, &sink);
    if (rc == TINYMCP_NEED_INPUT) {
        input_required(r);
        return;
    }
    if (rc == TINYMCP_ENOENT) {
        error_reply(r, -32602, "unknown prompt");
        return;
    }
    if (rc == TINYMCP_EINVAL) {
        error_reply(r, -32602, "a required argument is missing");
        return;
    }
    if (host_failed(r, rc, "the host could not render the prompt"))
        return;
    if (sink.cut)
        tinymcp_jw_raw(w, "\\n[cut]");
    tinymcp_jw_raw(w, "\"}}]");
    end_result(r);
}

int tinymcp_complete_add(tinymcp_completion_t *c, const char *value)
{
    tinymcp_jw_t *w = c->w;
    size_t mark = w->len;

    c->offered++;
    if (c->count >= TINYMCP_COMPLETE_MAX)
        return TINYMCP_EFULL;
    tinymcp_jw_sep(w);
    tinymcp_jw_str(w, value);
    if (w->full || w->len > c->limit) {
        tinymcp_jw_reset(w, mark);
        return TINYMCP_EFULL;
    }
    c->count++;
    return 0;
}

static void do_complete(req_t *r)
{
    tinymcp_jw_t *w = &r->w;
    tinymcp_jv_t ref, kind, target, arg, an, av, ctx, ctx_args;
    tinymcp_args_t context;
    tinymcp_completion_t c;
    const char *ref_name, *arg_name, *value;
    int is_prompt, rc;

    if (!param(r, "ref", &ref) || ref.type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(&ref, "type", &kind) != 1) {
        error_reply(r, -32602, "ref missing");
        return;
    }
    if (tinymcp_json_is(&kind, "ref/prompt")) {
        is_prompt = 1;
    } else if (tinymcp_json_is(&kind, "ref/resource")) {
        is_prompt = 0;
    } else {
        error_reply(r, -32602, "ref.type must be ref/prompt or ref/resource");
        return;
    }
    if (tinymcp_json_get(&ref, is_prompt ? "name" : "uri", &target) != 1 ||
        !param(r, "argument", &arg) || arg.type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(&arg, "name", &an) != 1 ||
        tinymcp_json_get(&arg, "value", &av) != 1) {
        error_reply(r, -32602, "ref or argument incomplete");
        return;
    }
    ctx_args.type = TINYMCP_J_NONE;
    if (param(r, "context", &ctx) && ctx.type == TINYMCP_J_OBJECT &&
        tinymcp_json_get(&ctx, "arguments", &ctx_args) != 1)
        ctx_args.type = TINYMCP_J_NONE;
    /* Every value is located; now each is decoded where it lies. */
    if (collect_object(&ctx_args, &context) < 0) {
        error_reply(r, -32602, "context.arguments must be an object of strings");
        return;
    }
    ref_name = decoded(&target);
    arg_name = decoded(&an);
    value = decoded(&av);
    if (!ref_name || !arg_name || !value) {
        error_reply(r, -32602, "ref and argument take strings");
        return;
    }
    begin_result(r);
    tinymcp_jw_key(w, "completion");
    tinymcp_jw_raw(w, "{\"values\":[");
    c.w = w;
    c.limit = r->t->out_cap - TAIL_RESERVE;
    c.count = 0;
    c.offered = 0;
    rc = r->t->host->complete(r->t->ctx, is_prompt, ref_name, arg_name, value,
                              &context, &c);
    if (rc == TINYMCP_ENOENT) {
        error_reply(r, -32602, is_prompt ? "unknown prompt" : "unknown template");
        return;
    }
    if (host_failed(r, rc, "the host could not complete"))
        return;
    tinymcp_jw_raw(w, "]");
    tinymcp_jw_key(w, "total");
    tinymcp_jw_ulong(w, c.offered);
    tinymcp_jw_key(w, "hasMore");
    tinymcp_jw_raw(w, c.offered > c.count ? "true" : "false");
    tinymcp_jw_raw(w, "}");
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
    unsigned want_lc = 0, nuris = 0, needed, first = 0, i;  /* want_lc: LC_* */
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
        want_lc |= TINYMCP_LC_RESOURCES;
    if (tinymcp_json_get(&filter, "promptsListChanged", &v) == 1 &&
        v.type == TINYMCP_J_TRUE && t->host->prompt_list)
        want_lc |= TINYMCP_LC_PROMPTS;
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
            t->subs[first].listchanged = (unsigned char)want_lc;
    }
    /* The acknowledgment is the first message on the stream. */
    tinymcp_jw_reset(w, 0);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"method\":"
                      "\"notifications/subscriptions/acknowledged\","
                      "\"params\":{\"_meta\":{\"" META_SUB "\":");
    tinymcp_jw_raw(w, id);
    tinymcp_jw_raw(w, "},\"notifications\":{");
    if (want_lc & TINYMCP_LC_RESOURCES) {
        tinymcp_jw_key(w, "resourcesListChanged");
        tinymcp_jw_raw(w, "true");
    }
    if (want_lc & TINYMCP_LC_PROMPTS) {
        tinymcp_jw_key(w, "promptsListChanged");
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

/* Drop the kept call; tell the host when the client cancelled it. */
static void later_drop(tinymcp_t *t, unsigned i, int cancelled)
{
    memset(&t->later[i], 0, sizeof t->later[i]);
    if (cancelled && t->host->cancel)
        t->host->cancel(t->ctx, i + 1);
}

static void do_notification(req_t *r)
{
    tinymcp_t *t = r->t;
    tinymcp_jv_t v;
    char id[TINYMCP_ID_MAX];
    unsigned i;

    if (strcmp(r->method, "notifications/cancelled") ||
        !param(r, "requestId", &v) || id_text(&v, id, sizeof id) != 0)
        return;
    for (i = 0; i < TINYMCP_LATER_MAX; i++) {
        if (t->later[i].used && !strcmp(t->later[i].id, id)) {
            later_drop(t, i, 1);
            return;
        }
    }
    listen_end(t, id, 0);
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
    else if (!strcmp(m, "prompts/list") && r->t->host->prompt_list)
        do_prompts_list(r);
    else if (!strcmp(m, "prompts/get") && r->t->host->prompt_get)
        do_prompts_get(r);
    else if (!strcmp(m, "completion/complete") && r->t->host->complete)
        do_complete(r);
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
    t->elicits = 0;
    t->in_responses.type = TINYMCP_J_NONE;
    t->in_state.type = TINYMCP_J_NONE;
    t->el_key = t->el_message = t->el_schema = t->el_state = NULL;
    t->cur_id.type = TINYMCP_J_NONE;
    t->cur_token.type = TINYMCP_J_NONE;
    t->cur_meta.type = TINYMCP_J_NONE;
    t->cur_modern = 0;
    t->deferred = 0;
    t->structured = NULL;
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
        {
            tinymcp_jv_t el;

            t->elicits = tinymcp_json_get(&v, "elicitation", &el) == 1 &&
                         el.type == TINYMCP_J_OBJECT;
        }
        /* A retry after input_required carries the answers and the state;
         * they are read in place, never decoded there. */
        if (!param(&r, "inputResponses", &t->in_responses))
            t->in_responses.type = TINYMCP_J_NONE;
        if (!param(&r, "requestState", &t->in_state))
            t->in_state.type = TINYMCP_J_NONE;
        /* Each request names its own client in this revision. */
        if (tinymcp_json_get(&meta, META_CLI, &v) == 1)
            keep_client(t, &v);
        else
            t->client[0] = 0;
    }
    /* Kept in place: the token for a call the host may keep open, the
     * whole object for tinymcp_meta(). */
    if (param(&r, "_meta", &meta) && meta.type == TINYMCP_J_OBJECT) {
        t->cur_meta = meta;
        if (tinymcp_json_get(&meta, "progressToken", &v) == 1 &&
            (v.type == TINYMCP_J_STRING || v.type == TINYMCP_J_NUMBER))
            t->cur_token = v;
    }
    t->cur_id = r.id;
    t->cur_modern = (unsigned char)r.modern;
    dispatch(&r);
out:
    t->structured = NULL;
    t->cur_id.type = TINYMCP_J_NONE;
    t->cur_token.type = TINYMCP_J_NONE;
    t->cur_meta.type = TINYMCP_J_NONE;
    t->deferred = 0;
    t->busy = 0;
    t->elicits = 0;
    t->in_responses.type = TINYMCP_J_NONE;
    t->in_state.type = TINYMCP_J_NONE;
    t->el_key = t->el_message = t->el_schema = t->el_state = NULL;
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
        t->pending_lc |= TINYMCP_LC_RESOURCES;
        return;
    }
    notify_listchanged(t, TINYMCP_LC_RESOURCES);
}

void tinymcp_prompts_changed(tinymcp_t *t)
{
    if (t->busy) {
        t->pending_lc |= TINYMCP_LC_PROMPTS;
        return;
    }
    notify_listchanged(t, TINYMCP_LC_PROMPTS);
}

void tinymcp_closed(tinymcp_t *t)
{
    unsigned i;

    for (i = 0; i < t->max_subs; i++)
        if (t->subs[i].used)
            slot_clear(t, i);
    /* Nobody is left to answer: the host stops the work. */
    for (i = 0; i < TINYMCP_LATER_MAX; i++)
        if (t->later[i].used)
            later_drop(t, i, 1);
    t->era = TINYMCP_ERA_NONE;
    t->legacy_version = NULL;
    t->client[0] = 0;
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

const char *tinymcp_client(const tinymcp_t *t)
{
    return t->client;
}

int tinymcp_meta(const tinymcp_t *t, const char *key, char *buf, size_t cap)
{
    tinymcp_jv_t v;

    /* Copied, then decoded in the copy: the message is not changed. */
    if (t->cur_meta.type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(&t->cur_meta, key, &v) != 1 ||
        v.type != TINYMCP_J_STRING || v.len + 1 > cap)
        return -1;
    memcpy(buf, v.at, v.len);
    return tinymcp_json_decode(buf, v.len);
}

int tinymcp_client_elicits(const tinymcp_t *t)
{
    return t->elicits;
}

int tinymcp_elicit(tinymcp_t *t, const char *key, const char *message,
                   const char *schema, const char *state)
{
    if (!t->elicits || !key || !message || !schema)
        return -1;
    t->el_key = key;
    t->el_message = message;
    t->el_schema = schema;
    t->el_state = state;
    return 0;
}

/* The response object under key in this request's inputResponses. */
static int input_response(const tinymcp_t *t, const char *key, tinymcp_jv_t *out)
{
    return t->in_responses.type == TINYMCP_J_OBJECT &&
           tinymcp_json_get(&t->in_responses, key, out) == 1 &&
           out->type == TINYMCP_J_OBJECT;
}

int tinymcp_input_action(const tinymcp_t *t, const char *key)
{
    tinymcp_jv_t resp, action;

    if (!input_response(t, key, &resp) ||
        tinymcp_json_get(&resp, "action", &action) != 1)
        return TINYMCP_INPUT_NONE;
    if (tinymcp_json_is(&action, "accept"))
        return TINYMCP_INPUT_ACCEPT;
    if (tinymcp_json_is(&action, "decline"))
        return TINYMCP_INPUT_DECLINE;
    if (tinymcp_json_is(&action, "cancel"))
        return TINYMCP_INPUT_CANCEL;
    return TINYMCP_INPUT_NONE;
}

int tinymcp_input_bool(const tinymcp_t *t, const char *key, const char *field)
{
    tinymcp_jv_t resp, content, v;

    if (!input_response(t, key, &resp) ||
        tinymcp_json_get(&resp, "content", &content) != 1 ||
        content.type != TINYMCP_J_OBJECT ||
        tinymcp_json_get(&content, field, &v) != 1)
        return -1;
    if (v.type == TINYMCP_J_TRUE)
        return 1;
    if (v.type == TINYMCP_J_FALSE)
        return 0;
    return -1;
}

int tinymcp_request_state(const tinymcp_t *t, char *buf, size_t cap)
{
    if (t->in_state.type != TINYMCP_J_STRING || t->in_state.len + 1 > cap)
        return -1;
    memcpy(buf, t->in_state.at, t->in_state.len);
    return tinymcp_json_decode(buf, t->in_state.len);
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

/* Templates */

static int name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* The length of the name when s opens a {{name}} placeholder, else 0. */
static size_t placeholder(const char *s, size_t n)
{
    size_t i;

    if (n < 5 || s[0] != '{' || s[1] != '{')
        return 0;
    for (i = 2; i < n && i - 2 < TINYMCP_PNAME_MAX - 1 && name_char(s[i]); i++)
        ;
    if (i == 2 || i + 1 >= n || s[i] != '}' || s[i + 1] != '}')
        return 0;
    return i - 2;
}

/* The string argument named by name[0..n), or NULL. */
static const char *arg_strn(const tinymcp_args_t *args, const char *name,
                            size_t n)
{
    unsigned i;

    for (i = 0; i < args->n; i++)
        if (strlen(args->a[i].key) == n && !memcmp(args->a[i].key, name, n))
            return args->a[i].str;
    return NULL;
}

unsigned tinymcp_placeholders(const char *text, size_t len,
                              char names[][TINYMCP_PNAME_MAX], unsigned max)
{
    unsigned count = 0, j;
    size_t i, k;

    for (i = 0; i < len; i++) {
        k = placeholder(text + i, len - i);
        if (!k)
            continue;
        for (j = 0; j < count; j++)
            if (strlen(names[j]) == k && !memcmp(names[j], text + i + 2, k))
                break;
        if (j == count && count < max) {
            memcpy(names[count], text + i + 2, k);
            names[count][k] = 0;
            count++;
        }
        i += k + 3;
    }
    return count;
}

int tinymcp_render(tinymcp_sink_t *sink, const char *text, size_t len,
                   const tinymcp_args_t *args)
{
    size_t i, k;
    const char *v;

    for (i = 0; i < len; i++) {
        k = placeholder(text + i, len - i);
        if (k && !arg_strn(args, text + i + 2, k))
            return TINYMCP_EINVAL;
        if (k)
            i += k + 3;
    }
    for (i = 0; i < len; i++) {
        k = placeholder(text + i, len - i);
        if (!k) {
            tinymcp_sink_putc(sink, text[i]);
            continue;
        }
        v = arg_strn(args, text + i + 2, k);
        tinymcp_sink_puts(sink, v);
        i += k + 3;
    }
    return 0;
}

/* Answering later */

void tinymcp_structured(tinymcp_t *t, const char *json)
{
    t->structured = json;
}

unsigned tinymcp_defer(tinymcp_t *t)
{
    tinymcp_later_t *l;
    unsigned i;

    if (!t->in_call)
        return 0;
    if (t->deferred)
        return t->deferred;
    for (i = 0; i < TINYMCP_LATER_MAX; i++) {
        l = &t->later[i];
        if (l->used)
            continue;
        if (id_text(&t->cur_id, l->id, sizeof l->id) != 0)
            return 0;
        /* A token too long to keep: the call runs without progress. */
        if (t->cur_token.type == TINYMCP_J_NONE ||
            id_text(&t->cur_token, l->token, sizeof l->token) != 0)
            l->token[0] = 0;
        l->modern = t->cur_modern;
        l->used = 1;
        t->deferred = (unsigned char)(i + 1);
        return i + 1;
    }
    return 0;
}

static tinymcp_later_t *later_of(tinymcp_t *t, unsigned call)
{
    if (call == 0 || call > TINYMCP_LATER_MAX || !t->later[call - 1].used)
        return NULL;
    return &t->later[call - 1];
}

int tinymcp_progress(tinymcp_t *t, unsigned call, unsigned long done,
                     unsigned long total, const char *message)
{
    tinymcp_later_t *l = later_of(t, call);
    tinymcp_jw_t w;

    if (!l || !l->token[0] || t->busy)
        return -1;
    tinymcp_jw_init(&w, t->out, t->out_cap);
    tinymcp_jw_raw(&w, "{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"notifications/progress\",\"params\":{"
                       "\"progressToken\":");
    tinymcp_jw_raw(&w, l->token);
    tinymcp_jw_key(&w, "progress");
    tinymcp_jw_ulong(&w, done);
    if (total) {
        tinymcp_jw_key(&w, "total");
        tinymcp_jw_ulong(&w, total);
    }
    if (message) {
        tinymcp_jw_key(&w, "message");
        tinymcp_jw_str(&w, message);
    }
    tinymcp_jw_raw(&w, "}}");
    emit(t, &w);
    return 0;
}

int tinymcp_result_begin(tinymcp_t *t, unsigned call, tinymcp_sink_t *text)
{
    tinymcp_later_t *l = later_of(t, call);
    tinymcp_jw_t *w = &t->later_w;

    if (!l || t->busy)
        return -1;
    /* Notifications wait until the result is out. */
    t->busy = 1;
    t->later_open = (unsigned char)call;
    tinymcp_jw_init(w, t->out, t->out_cap);
    tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"id\":");
    tinymcp_jw_raw(w, l->id);
    tinymcp_jw_raw(w, ",\"result\":{");
    result_head(t, w, l->modern, 0);
    tinymcp_jw_key(w, "content");
    tinymcp_jw_raw(w, "[");
    t->later_mark = w->len;
    tinymcp_sink_init(text, w, t->out_cap - TAIL_RESERVE);
    tinymcp_sink_lazy(text, TINYMCP_SINK_TOOL);
    stream_content(t, w);
    return 0;
}

void tinymcp_result_end(tinymcp_t *t, unsigned call, tinymcp_sink_t *text,
                        int is_error)
{
    tinymcp_later_t *l = later_of(t, call);
    tinymcp_jw_t *w = &t->later_w;

    if (!l || t->later_open != call)
        return;
    end_content(t, w, text, t->later_mark, is_error);
    tinymcp_jw_raw(w, "}}");
    if (w->full && !w->sent) {
        tinymcp_jw_reset(w, 0);
        tinymcp_jw_raw(w, "{\"jsonrpc\":\"2.0\",\"id\":");
        tinymcp_jw_raw(w, l->id);
        tinymcp_jw_raw(w, ",\"error\":{\"code\":-32603,\"message\":"
                          "\"reply does not fit the out buffer\"}}");
        t->errors++;
    }
    emit(t, w);
    later_drop(t, call - 1, 0);
    t->structured = NULL;
    t->later_open = 0;
    t->busy = 0;
    flush_pending(t);
}
