/*
 * tinymcp v0.8
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp.h - the server: the host fills a table of tools, resources and a
 * send function, owns two buffers, and feeds bytes; the library answers in
 * the 2026-07-28 stateless protocol and the initialize-era one before it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TINYMCP_H
#define TINYMCP_H

#include <stddef.h>
#include <stdint.h>

#include "tinymcp_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TINYMCP_VERSION  "0.8.0"
#define TINYMCP_API      4             /* bumped on any change to tinymcp_host_t */
#define TINYMCP_PROTOCOL "2026-07-28"  /* the newest revision served */

#ifndef TINYMCP_SUBS_MAX
#define TINYMCP_SUBS_MAX 4    /* subscription slots in a tinymcp_t */
#endif
#ifndef TINYMCP_URI_MAX
#define TINYMCP_URI_MAX 96    /* a subscribed URI, with its terminator */
#endif
#ifndef TINYMCP_ID_MAX
#define TINYMCP_ID_MAX 32     /* a listen request's id as sent, with terminator */
#endif
#ifndef TINYMCP_CLIENT_MAX
#define TINYMCP_CLIENT_MAX 32 /* a client's name, with its terminator; longer is cut */
#endif
#ifndef TINYMCP_ARGS_MAX
#define TINYMCP_ARGS_MAX 8    /* tool or prompt arguments kept; later ones are dropped */
#endif
#ifndef TINYMCP_LATER_MAX
#define TINYMCP_LATER_MAX 2   /* tool calls kept open at once (tinymcp_defer) */
#endif
#define TINYMCP_COMPLETE_MAX 100   /* completion values in one reply, the spec's cap */

/* What a host function returns. */
#define TINYMCP_OK          0
#define TINYMCP_TOOL_ERROR  1     /* tool_call: the text is the error, isError true */
#define TINYMCP_NEED_INPUT  2     /* tool_call, resource_read, prompt_get: send the
                                     elicitation tinymcp_elicit() set instead */
#define TINYMCP_LATER       3     /* tool_call: tinymcp_defer() kept the call, and
                                     its result is sent later */
#define TINYMCP_ENOENT    (-1)    /* no such resource */
#define TINYMCP_EEFFECT   (-2)    /* reading would act; the client is told to use a tool */
#define TINYMCP_EINVAL    (-3)    /* an argument is missing or wrong */
#define TINYMCP_EFULL     TINYMCP_JSON_EFULL
#define TINYMCP_EIO       (-5)    /* the host could not do it */
/* A value at or below -32000 is sent as that JSON-RPC error code. */

typedef enum tinymcp_era {
    TINYMCP_ERA_NONE = 0,   /* nothing decided yet */
    TINYMCP_ERA_MODERN,     /* a request with _meta.protocolVersion has been served */
    TINYMCP_ERA_LEGACY      /* initialize has been answered */
} tinymcp_era_t;

typedef struct tinymcp_tool {
    const char *name;
    const char *description;   /* may be NULL */
    const char *input_schema;  /* a JSON Schema object as JSON text */
    const char *output_schema; /* its structuredContent's schema; may be NULL */
} tinymcp_tool_t;

typedef struct tinymcp_template {
    const char *uri_template;  /* RFC 6570 */
    const char *name;
    const char *description;   /* may be NULL */
} tinymcp_template_t;

/** One tool argument, decoded; str is set for a string, num for a number. */
typedef struct tinymcp_arg {
    const char *key;
    const char *str;
    const char *raw;
    size_t raw_len;
    long num;
    tinymcp_jtype_t type;
} tinymcp_arg_t;

typedef struct tinymcp_args {
    unsigned n;
    tinymcp_arg_t a[TINYMCP_ARGS_MAX];
} tinymcp_args_t;

/** NULL unless the argument is present and a string. */
const char *tinymcp_arg_str(const tinymcp_args_t *args, const char *key);
/** dflt unless the argument is present and a number. */
long tinymcp_arg_int(const tinymcp_args_t *args, const char *key, long dflt);
/** dflt unless the argument is present and true or false. */
int tinymcp_arg_bool(const tinymcp_args_t *args, const char *key, int dflt);

#define TINYMCP_PNAME_MAX 32   /* a {{name}} placeholder's name, with its terminator */

/**
 * The {{name}} placeholders of a prompt template, each name once, in the
 * order of first use, at most max of them.  A name is letters, digits and
 * '_', up to TINYMCP_PNAME_MAX - 1 of them; any other {{ is text.
 * @return the number of names written.
 */
unsigned tinymcp_placeholders(const char *text, size_t len,
                              char names[][TINYMCP_PNAME_MAX], unsigned max);

/**
 * Write a prompt template into the sink with each {{name}} replaced by the
 * string argument of that name.
 * @return 0, or TINYMCP_EINVAL, with nothing written, when a placeholder
 *         has no string argument.
 */
int tinymcp_render(tinymcp_sink_t *sink, const char *text, size_t len,
                   const tinymcp_args_t *args);

/** The resources/list or prompts/list reply under construction. */
typedef struct tinymcp_list {
    tinymcp_jw_t *w;
    size_t limit;
    unsigned count;
} tinymcp_list_t;

/**
 * Add one resource entry; description may be NULL.
 * @return 0, or TINYMCP_EFULL when it did not fit, in which case the host
 *         returns this entry's cursor so the next page starts with it.
 */
int tinymcp_list_add(tinymcp_list_t *list, const char *uri, const char *name,
                     const char *description);

/** One argument a prompt takes; description may be NULL. */
typedef struct tinymcp_prompt_arg {
    const char *name;
    const char *description;
    unsigned char required;
} tinymcp_prompt_arg_t;

/**
 * Add one prompt entry to a prompts/list reply, with nargs arguments;
 * description may be NULL.
 * @return 0, or TINYMCP_EFULL as for tinymcp_list_add().
 */
int tinymcp_prompt_add(tinymcp_list_t *list, const char *name,
                       const char *description,
                       const tinymcp_prompt_arg_t *args, unsigned nargs);

/** The completion/complete reply under construction. */
typedef struct tinymcp_completion {
    tinymcp_jw_t *w;
    size_t limit;
    unsigned count;    /* values in the reply */
    unsigned offered;  /* values the host offered, kept or not */
} tinymcp_completion_t;

/**
 * Offer one completion value.  The reply keeps the first
 * TINYMCP_COMPLETE_MAX that fit and reports the rest through total and
 * hasMore.
 * @return 0, or TINYMCP_EFULL when this value was not kept.
 */
int tinymcp_complete_add(tinymcp_completion_t *c, const char *value);

/**
 * What the host provides.  Strings are kept by pointer, outlive the instance
 * and are read again for each reply.  A NULL resource_list, watch, prompt_list
 * or complete serves no resources, subscriptions, prompts or completion.
 */
typedef struct tinymcp_host {
    const char *name;          /* serverInfo.name */
    const char *version;       /* serverInfo.version */
    const char *instructions;  /* may be NULL */
    const tinymcp_tool_t *tools;
    unsigned ntools;
    const tinymcp_template_t *templates;
    unsigned ntemplates;
    /*
     * Add entries from cursor on (0 starts) with tinymcp_list_add() until
     * one does not fit; return that entry's cursor (1 or more), 0 after the
     * last entry, TINYMCP_EFULL when not even the first fit, or another
     * negative code.
     */
    int (*resource_list)(void *ctx, unsigned cursor, tinymcp_list_t *list);
    /*
     * Write the resource's text into the sink; line and bytes are the
     * URI's ?line=&bytes= query, 0 when absent.  TINYMCP_ENOENT,
     * TINYMCP_EEFFECT, TINYMCP_EINVAL or 0.
     */
    int (*resource_read)(void *ctx, const char *uri, unsigned line,
                         unsigned bytes, tinymcp_sink_t *text);
    /*
     * Run the tool named name, which is one of tools[]; write its output.
     * TINYMCP_OK, TINYMCP_TOOL_ERROR, TINYMCP_EINVAL, or a code.
     */
    int (*tool_call)(void *ctx, const char *name, const tinymcp_args_t *args,
                     tinymcp_sink_t *text);
    /*
     * Start watching uri under slot; later changes are reported with
     * tinymcp_updated(slot).  Nonzero refuses the subscription.
     */
    int (*watch)(void *ctx, const char *uri, unsigned slot);
    void (*unwatch)(void *ctx, unsigned slot);
    /* One whole message without a trailing newline; nonzero means not sent. */
    int (*send)(void *ctx, const char *msg, size_t len);
    const char *title;         /* serverInfo.title; may be NULL */
    /*
     * Add prompts from cursor on with tinymcp_prompt_add(); returns as
     * resource_list does.
     */
    int (*prompt_list)(void *ctx, unsigned cursor, tinymcp_list_t *list);
    /*
     * Write the prompt's text, its arguments substituted, into the sink; it
     * becomes one user message.  TINYMCP_ENOENT for no such prompt,
     * TINYMCP_EINVAL for a required argument missing, or 0.
     */
    int (*prompt_get)(void *ctx, const char *name, const tinymcp_args_t *args,
                      tinymcp_sink_t *text);
    /*
     * Offer completions with tinymcp_complete_add() for the argument arg,
     * typed so far as value, of the prompt named ref (is_prompt) or of the
     * resource template ref; context holds the arguments already chosen.
     * TINYMCP_ENOENT for an unknown prompt or template, or 0.
     */
    int (*complete)(void *ctx, int is_prompt, const char *ref, const char *arg,
                    const char *value, const tinymcp_args_t *context,
                    tinymcp_completion_t *c);
    /*
     * The client cancelled the kept call (tinymcp_defer()); stop its work.
     * The call is gone when this returns and no result is sent for it.
     * May be NULL.
     */
    void (*cancel)(void *ctx, unsigned call);
    /*
     * One piece of a message, last set on the piece that ends it.  With it,
     * a resource's text, a tool's content and a kept call's result go out
     * as they are written, past the out buffer; without it they are cut
     * there.  Messages that fit still go whole through send.  May be NULL.
     */
    int (*send_part)(void *ctx, const char *bytes, size_t len, int last);
} tinymcp_host_t;

#define TINYMCP_LC_RESOURCES 1u   /* listchanged: resources/list_changed */
#define TINYMCP_LC_PROMPTS   2u   /* listchanged: prompts/list_changed */

typedef struct tinymcp_sub {
    char uri[TINYMCP_URI_MAX];   /* empty: a list-changed listen alone */
    char id[TINYMCP_ID_MAX];     /* the listen request's id as sent; empty: legacy */
    unsigned seq;
    unsigned char used;
    unsigned char listchanged;   /* TINYMCP_LC_* bits */
} tinymcp_sub_t;

/** A tool call kept open: its id and progress token as the client sent them. */
typedef struct tinymcp_later {
    char id[TINYMCP_ID_MAX];
    char token[TINYMCP_ID_MAX];  /* _meta.progressToken; empty: none */
    unsigned char modern;
    unsigned char used;
} tinymcp_later_t;

/** One server instance: one stream, one request handled at a time. */
typedef struct tinymcp {
    const tinymcp_host_t *host;
    void *ctx;
    char *in;
    size_t in_cap;
    size_t in_len;
    unsigned char in_drop;      /* a line longer than in fits is being skipped */
    char *out;
    size_t out_cap;
    unsigned max_subs;
    tinymcp_sub_t subs[TINYMCP_SUBS_MAX];
    unsigned sub_seq;
    tinymcp_era_t era;
    const char *legacy_version; /* the version initialize settled on */
    char client[TINYMCP_CLIENT_MAX]; /* the requesting client's name, or "" */
    unsigned char elicits;      /* this request's client can be asked (input_required) */
    tinymcp_jv_t in_responses;  /* this request's inputResponses, or J_NONE */
    tinymcp_jv_t in_state;      /* this request's requestState, or J_NONE */
    const char *el_key;         /* the elicitation a host asked for, until it returns */
    const char *el_message;
    const char *el_schema;
    const char *el_state;
    tinymcp_jv_t cur_id;        /* the request being handled, its id */
    tinymcp_jv_t cur_token;     /* and its _meta.progressToken, or J_NONE */
    tinymcp_jv_t cur_meta;      /* and its params._meta object, or J_NONE */
    unsigned char cur_modern;
    unsigned char in_call;      /* inside tool_call: tinymcp_defer() may keep it */
    unsigned char deferred;     /* the handle tinymcp_defer() gave this call, or 0 */
    tinymcp_later_t later[TINYMCP_LATER_MAX];
    unsigned char later_open;   /* the call whose result is being written, or 0 */
    tinymcp_jw_t later_w;
    size_t later_mark;          /* where that result's content begins */
    const char *structured;     /* the tool result's structuredContent, or NULL */
    unsigned char busy;         /* inside a request: notifications wait */
    unsigned pending;           /* slots whose update waits, one bit each */
    unsigned char pending_lc;   /* TINYMCP_LC_* notices that wait */
    unsigned requests;
    unsigned errors;
    unsigned dropped;           /* messages send() refused */
} tinymcp_t;

/**
 * Bind an instance to its host and buffers.  in holds one message as it
 * arrives (in_cap - 1 bytes at most); out holds one reply.  max_subs is
 * clamped to TINYMCP_SUBS_MAX.
 */
void tinymcp_init(tinymcp_t *t, const tinymcp_host_t *host, void *ctx,
                  char *in, size_t in_cap, char *out, size_t out_cap,
                  unsigned max_subs);

/**
 * Bytes from a stream, one message per line.  Replies and notifications go
 * out through host->send before this returns.
 * @return the number of messages handled.
 */
int tinymcp_feed(tinymcp_t *t, const void *bytes, size_t len);

/**
 * One whole message from a framing transport; a trailing newline is fine.
 * @return 0, or -1 when it is larger than the in buffer (a parse error
 *         reply has then been sent).
 */
int tinymcp_message(tinymcp_t *t, const void *msg, size_t len);

/**
 * One whole message in the host's own buffer, decoded there: msg[len]
 * must be writable, and the bytes are not the host's afterwards.  For a
 * transport that already holds the message and gave no in buffer.
 */
void tinymcp_handle(tinymcp_t *t, char *msg, size_t len);

/** The resource under slot changed: notifications/resources/updated. */
void tinymcp_updated(tinymcp_t *t, unsigned slot);

/** The set of resources changed: notifications/resources/list_changed. */
void tinymcp_listchanged(tinymcp_t *t);

/** The set of prompts changed: notifications/prompts/list_changed. */
void tinymcp_prompts_changed(tinymcp_t *t);

/** The transport is gone: subscriptions end and the era is forgotten. */
void tinymcp_closed(tinymcp_t *t);

/** "none", "modern" or "legacy". */
const char *tinymcp_era_name(const tinymcp_t *t);

/**
 * The name the client gave in clientInfo: its initialize for the handshake
 * era, the request's _meta for the current revision; "" when it gave none.
 */
const char *tinymcp_client(const tinymcp_t *t);

/*
 * Asking the person: a host function that needs a yes before it acts calls
 * tinymcp_elicit() and returns TINYMCP_NEED_INPUT; the client shows the form
 * and sends the request again, with the answers and the state, which the
 * host reads through the tinymcp_input_* calls while it runs.
 */

/**
 * The string member key of the request's params._meta, decoded into buf,
 * while a host function serves it: a credential such as "org.tinymcp/key".
 * @return its length, or -1 when it is absent, not a string, or does not
 *         fit cap - 1.
 */
int tinymcp_meta(const tinymcp_t *t, const char *key, char *buf, size_t cap);

/** True when the request being served can carry input_required: the
 *  2026-07-28 revision, from a client that declared elicitation. */
int tinymcp_client_elicits(const tinymcp_t *t);

/**
 * Describe the form the client must show: key names it in inputRequests,
 * schema is a flat JSON Schema object as text, state comes back verbatim
 * as requestState (NULL for none).  The strings must outlive the host call.
 * @return 0, or -1 when the request cannot carry input_required.
 */
int tinymcp_elicit(tinymcp_t *t, const char *key, const char *message,
                   const char *schema, const char *state);

#define TINYMCP_INPUT_NONE    0   /* no response under that key */
#define TINYMCP_INPUT_ACCEPT  1
#define TINYMCP_INPUT_DECLINE 2
#define TINYMCP_INPUT_CANCEL  3

/** The action of the response under key in this request's inputResponses. */
int tinymcp_input_action(const tinymcp_t *t, const char *key);

/** A boolean field of that response's content: 1, 0, or -1 when absent. */
int tinymcp_input_bool(const tinymcp_t *t, const char *key, const char *field);

/**
 * This request's requestState, decoded into buf.
 * @return its length, or -1 when it is absent or does not fit cap - 1.
 */
int tinymcp_request_state(const tinymcp_t *t, char *buf, size_t cap);

/*
 * Answering later: a tool whose work outlasts the request calls
 * tinymcp_defer() and returns TINYMCP_LATER.  The library keeps the call's
 * id and progress token; the host reports progress, then writes the result
 * between tinymcp_result_begin() and tinymcp_result_end().  None of these
 * may be called from inside a host function: they return -1 there.
 */

/**
 * Inside tool_call, or between tinymcp_result_begin() and _end(): json, an
 * object as text that outlives the call, becomes the result's
 * structuredContent; it must fit a tool's output_schema when it has one.
 */
void tinymcp_structured(tinymcp_t *t, const char *json);

/** Inside tool_call: keep this call open.  Its handle, 1 or more, or 0
 *  when TINYMCP_LATER_MAX calls are kept or its id does not fit. */
unsigned tinymcp_defer(tinymcp_t *t);

/**
 * notifications/progress for a kept call: done so far of total (0 when
 * unknown), message NULL for none.
 * @return 0 when sent; -1 for no such call, a call without a progress
 *         token, or inside a host function.
 */
int tinymcp_progress(tinymcp_t *t, unsigned call, unsigned long done,
                     unsigned long total, const char *message);

/** Open the kept call's result, text content into text.
 *  @return 0, or -1 for no such call or inside a host function. */
int tinymcp_result_begin(tinymcp_t *t, unsigned call, tinymcp_sink_t *text);

/** Send the result tinymcp_result_begin() opened, with that sink; the call
 *  is then gone.  isError is true for is_error or for text that was cut. */
void tinymcp_result_end(tinymcp_t *t, unsigned call, tinymcp_sink_t *text,
                        int is_error);

#ifdef __cplusplus
}
#endif

#endif /* TINYMCP_H */
