/*
 * tinymcp v0.4
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * test_tinymcp.c - the protocol against a fake host: both eras, every
 * method, every error code, the buffers' edges, subscriptions and their
 * eviction, prompts and completion.  Each check is known to fail on a mutant (tests/mutants.sh).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinymcp.h"

/* The fake host */

#define SENT_MAX 16
#define SENT_LEN 4096

static char sent[SENT_MAX][SENT_LEN];
static int nsent;
static int send_fail;
static char watched[TINYMCP_SUBS_MAX][64];
static int unwatched[TINYMCP_SUBS_MAX];
static unsigned last_line, last_bytes;
static tinymcp_t *touch_target;   /* the instance the touch tool notifies */

static int fake_send(void *ctx, const char *msg, size_t len)
{
    (void)ctx;
    if (send_fail)
        return -1;
    if (nsent < SENT_MAX && len < SENT_LEN) {
        memcpy(sent[nsent], msg, len);
        sent[nsent][len] = 0;
    }
    nsent++;
    return 0;
}

static const char *const res_uris[] = {
    "test:///a", "test:///b", "test:///effect"
};
#define NRES (sizeof res_uris / sizeof res_uris[0])

static int fake_list(void *ctx, unsigned cursor, tinymcp_list_t *list)
{
    unsigned i;

    (void)ctx;
    for (i = cursor; i < NRES; i++)
        if (tinymcp_list_add(list, res_uris[i], res_uris[i] + 8,
                             i == 0 ? "the first" : NULL) != 0)
            return i ? (int)i : TINYMCP_EFULL;
    return 0;
}

static int guarded(tinymcp_sink_t *text);

static int fake_read(void *ctx, const char *uri, unsigned line, unsigned bytes,
                     tinymcp_sink_t *text)
{
    (void)ctx;
    last_line = line;
    last_bytes = bytes;
    if (!strcmp(uri, "test:///a")) {
        if (line || bytes) {
            char buf[64];

            snprintf(buf, sizeof buf, "line=%u bytes=%u", line, bytes);
            tinymcp_sink_puts(text, buf);
        } else {
            tinymcp_sink_puts(text, "alpha\n\"quoted\"");
        }
        return 0;
    }
    if (!strcmp(uri, "test:///b")) {
        tinymcp_sink_puts(text, "bravo");
        return 0;
    }
    if (!strcmp(uri, "test:///effect"))
        return TINYMCP_EEFFECT;
    if (!strcmp(uri, "test:///guarded"))
        return guarded(text);
    if (!strcmp(uri, "test:///long")) {
        int i;

        for (i = 0; i < 20000; i++)
            tinymcp_sink_putc(text, 'x');
        return 0;
    }
    return TINYMCP_ENOENT;
}

static const tinymcp_tool_t tools[] = {
    { "echo", "says text back",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
      "\"required\":[\"text\"]}" },
    { "add", NULL,
      "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"integer\"},"
      "\"b\":{\"type\":\"integer\"}}}" },
    { "fail", "always a tool error", "{\"type\":\"object\"}" },
    { "boom", "a protocol error", "{\"type\":\"object\"}" },
    { "long", "more than fits", "{\"type\":\"object\"}" },
    { "touch", "notifies slot 0 from inside the call", "{\"type\":\"object\"}" },
    { "flag", "a boolean", "{\"type\":\"object\"}" },
    { "reprompt", "prompts changed, from inside the call", "{\"type\":\"object\"}" },
    { "guarded", "asks the person first", "{\"type\":\"object\"}" },
    { "lazy", "needs input it does not describe", "{\"type\":\"object\"}" },
};
#define NTOOLS (sizeof tools / sizeof tools[0])

static const tinymcp_template_t templates[] = {
    { "test:///{+path}", "a path", "any path" },
    { "test:///{+path}{?line,bytes}", "a page", NULL },
};

#define CONFIRM_SCHEMA "{\"type\":\"object\",\"properties\":{\"confirm\":" \
                       "{\"type\":\"boolean\"}},\"required\":[\"confirm\"]}"

/* A yes through input_required: done after an accepted confirm under the
 * state "s1", refused after a decline, asked again otherwise. */
static int guarded(tinymcp_sink_t *text)
{
    char state[8];
    int action = tinymcp_input_action(touch_target, "confirm");

    if (!tinymcp_client_elicits(touch_target)) {
        tinymcp_sink_puts(text, "cannot ask");
        return TINYMCP_TOOL_ERROR;
    }
    if (action == TINYMCP_INPUT_DECLINE || action == TINYMCP_INPUT_CANCEL) {
        tinymcp_sink_puts(text, action == TINYMCP_INPUT_DECLINE ? "declined"
                                                                : "cancelled");
        return TINYMCP_TOOL_ERROR;
    }
    if (action == TINYMCP_INPUT_ACCEPT &&
        tinymcp_input_bool(touch_target, "confirm", "confirm") == 1 &&
        tinymcp_request_state(touch_target, state, sizeof state) == 2 &&
        !strcmp(state, "s1")) {
        tinymcp_sink_puts(text, "done");
        return TINYMCP_OK;
    }
    tinymcp_elicit(touch_target, "confirm", "Really do it?", CONFIRM_SCHEMA, "s1");
    return TINYMCP_NEED_INPUT;
}

static int fake_tool(void *ctx, const char *name, const tinymcp_args_t *args,
                     tinymcp_sink_t *text)
{
    (void)ctx;
    if (!strcmp(name, "echo")) {
        const char *s = tinymcp_arg_str(args, "text");

        if (!s)
            return TINYMCP_EINVAL;
        tinymcp_sink_puts(text, s);
        return TINYMCP_OK;
    }
    if (!strcmp(name, "add")) {
        char buf[32];

        snprintf(buf, sizeof buf, "%ld",
                 tinymcp_arg_int(args, "a", 0) + tinymcp_arg_int(args, "b", 0));
        tinymcp_sink_puts(text, buf);
        return TINYMCP_OK;
    }
    if (!strcmp(name, "fail")) {
        tinymcp_sink_puts(text, "it failed");
        return TINYMCP_TOOL_ERROR;
    }
    if (!strcmp(name, "boom"))
        return -32050;
    if (!strcmp(name, "long")) {
        int i;

        for (i = 0; i < 20000; i++)
            tinymcp_sink_putc(text, 'y');
        return TINYMCP_OK;
    }
    if (!strcmp(name, "touch")) {
        tinymcp_updated(touch_target, 0);
        tinymcp_listchanged(touch_target);
        tinymcp_sink_puts(text, "touched");
        return TINYMCP_OK;
    }
    if (!strcmp(name, "guarded"))
        return guarded(text);
    if (!strcmp(name, "lazy"))
        return TINYMCP_NEED_INPUT;
    if (!strcmp(name, "reprompt")) {
        tinymcp_prompts_changed(touch_target);
        tinymcp_sink_puts(text, "reprompted");
        return TINYMCP_OK;
    }
    if (!strcmp(name, "flag")) {
        tinymcp_sink_puts(text, tinymcp_arg_bool(args, "on", 0) ? "on" : "off");
        return TINYMCP_OK;
    }
    return TINYMCP_EINVAL;
}

static int fake_watch(void *ctx, const char *uri, unsigned slot)
{
    (void)ctx;
    if (!strcmp(uri, "test:///nope"))
        return -1;
    strncpy(watched[slot], uri, sizeof watched[slot] - 1);
    watched[slot][sizeof watched[slot] - 1] = 0;
    return 0;
}

static void fake_unwatch(void *ctx, unsigned slot)
{
    (void)ctx;
    unwatched[slot]++;
    watched[slot][0] = 0;
}

/* Prompts: greet takes who (required) and mood; plain takes nothing. */
static const tinymcp_prompt_arg_t greet_args[] = {
    { "who", "whom to greet", 1 },
    { "mood", NULL, 0 },
};

static const char *const prompt_names[] = { "greet", "plain", "huge" };
#define NPROMPTS (sizeof prompt_names / sizeof prompt_names[0])

static int fake_prompt_list(void *ctx, unsigned cursor, tinymcp_list_t *list)
{
    unsigned i;

    (void)ctx;
    for (i = cursor; i < NPROMPTS; i++) {
        int rc = i == 0 ? tinymcp_prompt_add(list, "greet", "a greeting",
                                             greet_args, 2)
                        : tinymcp_prompt_add(list, prompt_names[i], NULL,
                                             NULL, 0);

        if (rc != 0)
            return i ? (int)i : TINYMCP_EFULL;
    }
    return 0;
}

static int fake_prompt_get(void *ctx, const char *name,
                           const tinymcp_args_t *args, tinymcp_sink_t *text)
{
    (void)ctx;
    if (!strcmp(name, "greet")) {
        const char *who = tinymcp_arg_str(args, "who");
        const char *mood = tinymcp_arg_str(args, "mood");

        if (!who)
            return TINYMCP_EINVAL;
        tinymcp_sink_puts(text, "Hello, ");
        tinymcp_sink_puts(text, who);
        if (mood) {
            tinymcp_sink_puts(text, " (");
            tinymcp_sink_puts(text, mood);
            tinymcp_sink_puts(text, ")");
        }
        return 0;
    }
    if (!strcmp(name, "plain")) {
        tinymcp_sink_puts(text, "Just text.\nTwo lines.");
        return 0;
    }
    if (!strcmp(name, "huge")) {
        int i;

        for (i = 0; i < 20000; i++)
            tinymcp_sink_putc(text, 'z');
        return 0;
    }
    return TINYMCP_ENOENT;
}

static const char *const people[] = { "ada", "alan", "grace" };

/* who of greet from people; path of test:///{+path} from p000..p149, or the
 * context's dir argument when it is set. */
static int fake_complete(void *ctx, int is_prompt, const char *ref,
                         const char *arg, const char *value,
                         const tinymcp_args_t *context, tinymcp_completion_t *c)
{
    size_t n = strlen(value);
    unsigned i;

    (void)ctx;
    if (is_prompt && !strcmp(ref, "greet")) {
        if (strcmp(arg, "who"))
            return 0;
        for (i = 0; i < sizeof people / sizeof people[0]; i++)
            if (!strncmp(people[i], value, n))
                tinymcp_complete_add(c, people[i]);
        return 0;
    }
    if (!is_prompt && !strcmp(ref, "test:///{+path}")) {
        const char *dir = tinymcp_arg_str(context, "dir");
        char v[8];

        if (dir) {
            tinymcp_complete_add(c, dir);
            return 0;
        }
        for (i = 0; i < 150; i++) {
            snprintf(v, sizeof v, "p%03u", i);
            if (!strncmp(v, value, n))
                tinymcp_complete_add(c, v);
        }
        return 0;
    }
    return TINYMCP_ENOENT;
}

static const tinymcp_host_t host = {
    "fake", "9.9", "Be nice to the fake.",
    tools, NTOOLS, templates, 2,
    fake_list, fake_read, fake_tool, fake_watch, fake_unwatch, fake_send,
    NULL, NULL, NULL, NULL
};

static const tinymcp_host_t host_full = {
    "fake", "9.9", "Be nice to the fake.",
    tools, NTOOLS, templates, 2,
    fake_list, fake_read, fake_tool, fake_watch, fake_unwatch, fake_send,
    "The Fake", fake_prompt_list, fake_prompt_get, fake_complete
};

static const tinymcp_host_t host_plain = {
    "plain", "1", NULL,
    tools, 1, NULL, 0,
    NULL, NULL, fake_tool, NULL, NULL, fake_send,
    NULL, NULL, NULL, NULL
};

/* The checks */

static int failures;
static int checks;

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CONTAINS(hay, needle) \
    check(strstr((hay), (needle)) != NULL, "contains " #needle, __LINE__)
#define LACKS(hay, needle) \
    check(strstr((hay), (needle)) == NULL, "lacks " #needle, __LINE__)
#define SAME(a, b) check(strcmp((a), (b)) == 0, "same " #b, __LINE__)

static void check(int ok, const char *what, int line)
{
    checks++;
    if (ok)
        return;
    failures++;
    printf("FAIL line %d: %s\n", line, what);
    if (nsent)
        printf("  last message: %s\n", sent[nsent - 1 < SENT_MAX ? nsent - 1 : SENT_MAX - 1]);
}

static char in_buf[2048], out_buf[4096];
static tinymcp_t t;

static void reset(const tinymcp_host_t *h, size_t out_cap, unsigned max_subs)
{
    memset(sent, 0, sizeof sent);
    memset(watched, 0, sizeof watched);
    memset(unwatched, 0, sizeof unwatched);
    nsent = 0;
    send_fail = 0;
    tinymcp_init(&t, h, NULL, in_buf, sizeof in_buf, out_buf, out_cap,
                 max_subs);
    touch_target = &t;
}

static const char *last(void)
{
    return nsent ? sent[nsent - 1] : "";
}

/* Feed one line; returns the number of messages sent for it. */
static int say(const char *json)
{
    int before = nsent;

    tinymcp_feed(&t, json, strlen(json));
    tinymcp_feed(&t, "\n", 1);
    return nsent - before;
}

#define META "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":" \
             "\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{}}"

static void modern_discover(void)
{
    reset(&host, sizeof out_buf, 4);
    CHECK(say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\","
              "\"params\":{" META "}}") == 1);
    CONTAINS(last(), "\"id\":1,\"result\":{\"resultType\":\"complete\"");
    CONTAINS(last(), "\"_meta\":{\"io.modelcontextprotocol/serverInfo\":"
                     "{\"name\":\"fake\",\"version\":\"9.9\"},"
                     "\"org.tinymcp/library\":\"tinymcp " TINYMCP_VERSION "\"}");
    CONTAINS(last(), "\"supportedVersions\":[\"2026-07-28\",\"2025-11-25\","
                     "\"2025-06-18\",\"2025-03-26\",\"2024-11-05\"]");
    CONTAINS(last(), "\"capabilities\":{\"tools\":{},\"resources\":"
                     "{\"subscribe\":true,\"listChanged\":true}}");
    CONTAINS(last(), "\"instructions\":\"Be nice to the fake.\"");
    CONTAINS(last(), "\"ttlMs\":3600000,\"cacheScope\":\"private\"}}");
    SAME(tinymcp_era_name(&t), "modern");

    /* A host without resources or instructions advertises neither. */
    reset(&host_plain, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"capabilities\":{\"tools\":{}}");
    LACKS(last(), "instructions");
}

static void modern_meta_rules(void)
{
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    CONTAINS(last(), "\"id\":2,\"error\":{\"code\":-32602");
    CONTAINS(last(), "protocolVersion");
    SAME(tinymcp_era_name(&t), "none");

    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":"
        "{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":"
        "\"2025-11-25\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    CONTAINS(last(), "\"error\":{\"code\":-32022,\"message\":");
    CONTAINS(last(), "\"data\":{\"supported\":[\"2026-07-28\",");
    CONTAINS(last(), "\"requested\":\"2025-11-25\"}");

    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\",\"params\":"
        "{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":"
        "\"2026-07-28\"}}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602");
    CONTAINS(last(), "clientCapabilities");

    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\",\"params\":"
        "{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":7,"
        "\"io.modelcontextprotocol/clientCapabilities\":{}}}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602");

    /* Legacy-only methods are unknown to a modern request. */
    say("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/subscribe\","
        "\"params\":{\"uri\":\"test:///a\"," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32601");
    say("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"nothing/here\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"id\":7,\"error\":{\"code\":-32601,\"message\":\"unknown method\"}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\",\"params\":{" META "}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"resultType\":"
                 "\"complete\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":"
                 "{\"name\":\"fake\",\"version\":\"9.9\"}}}}");
    CHECK(t.requests == 7 && t.errors == 6);
}

static void modern_tools(void)
{
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":\"t1\",\"method\":\"tools/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"id\":\"t1\",\"result\":");
    CONTAINS(last(), "\"tools\":[{\"name\":\"echo\",\"description\":\"says text back\","
                     "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":"
                     "{\"type\":\"string\"}},\"required\":[\"text\"]}},"
                     "{\"name\":\"add\",\"inputSchema\":");
    CONTAINS(last(), "{\"name\":\"flag\",");
    LACKS(last(), "nextCursor");
    CONTAINS(last(), "\"ttlMs\":3600000");

    say("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"echo\",\"arguments\":{\"text\":\"caf\\u00e9 \\ud83d\\ude00 "
        "\\\"q\\\" \\\\ \\n tab\\t\"}," META "}}");
    CONTAINS(last(), "\"result\":{\"resultType\":\"complete\",");
    CONTAINS(last(), "\"content\":[{\"type\":\"text\",\"text\":\"caf\xc3\xa9 "
                     "\xf0\x9f\x98\x80 \\\"q\\\" \\\\ \\n tab\\t\"}]}}");
    LACKS(last(), "isError");

    say("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"add\",\"arguments\":{\"a\":40,\"b\":-2}," META "}}");
    CONTAINS(last(), "\"text\":\"38\"}]}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"fail\"," META "}}");
    CONTAINS(last(), "\"text\":\"it failed\"}],\"isError\":true}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"boom\"," META "}}");
    CONTAINS(last(), "\"id\":12,\"error\":{\"code\":-32050,");

    say("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"nope\"," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"unknown tool\"}");

    say("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"echo\",\"arguments\":{\"text\":5}," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"invalid arguments\"}");

    say("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"long\"," META "}}");
    CONTAINS(last(), "yyyy\\n[cut]\"}],\"isError\":true}}");
    CHECK(strlen(last()) < sizeof out_buf);

    say("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"flag\",\"arguments\":{\"on\":true}," META "}}");
    CONTAINS(last(), "\"text\":\"on\"");
    say("{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"flag\",\"arguments\":{\"on\":\"yes\"}," META "}}");
    CONTAINS(last(), "\"text\":\"off\"");

    say("{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"echo\",\"arguments\":[1]," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602");
    say("{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"tools/call\",\"params\":{"
        META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"name missing\"}");
}

static void modern_paging(void)
{
    /* An out buffer that holds the first two tool entries, not the third. */
    reset(&host, 500, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"tools\":[{\"name\":\"echo\"");
    CONTAINS(last(), "{\"name\":\"add\"");
    CONTAINS(last(), "}],\"nextCursor\":\"2\",\"ttlMs\":3600000,\"cacheScope\":\"private\"}}");
    LACKS(last(), "\"name\":\"fail\"");
    CHECK(strlen(last()) < 500);
    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\","
        "\"params\":{\"cursor\":\"2\"," META "}}");
    CONTAINS(last(), "\"tools\":[{\"name\":\"fail\"");
    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\","
        "\"params\":{\"cursor\":\"x\"," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"unknown cursor\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\","
        "\"params\":{\"cursor\":\"99\"," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602");

    /* The same for resources, which the host pages. */
    reset(&host, 400, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"resources\":[{\"uri\":\"test:///a\",\"name\":\"a\","
                     "\"description\":\"the first\",\"mimeType\":\"text/plain\"}");
    CONTAINS(last(), "\"nextCursor\":\"2\"");
    say("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/list\","
        "\"params\":{\"cursor\":\"2\"," META "}}");
    CONTAINS(last(), "\"resources\":[{\"uri\":\"test:///effect\"");
    LACKS(last(), "nextCursor");

    /* Nothing fits: an error, not an empty page with a cursor. */
    reset(&host, 200, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32603");
    say("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"server/discover\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"id\":8,\"error\":{\"code\":-32603,\"message\":"
                     "\"reply does not fit the out buffer\"}}");
}

static void modern_resources(void)
{
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "{\"uri\":\"test:///b\",\"name\":\"b\",\"mimeType\":\"text/plain\"}");
    CONTAINS(last(), "}],\"ttlMs\":0,\"cacheScope\":\"private\"}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/templates/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"resourceTemplates\":[{\"uriTemplate\":\"test:///{+path}\","
                     "\"name\":\"a path\",\"description\":\"any path\","
                     "\"mimeType\":\"text/plain\"},{\"uriTemplate\":");

    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///a\"," META "}}");
    CONTAINS(last(), "\"contents\":[{\"uri\":\"test:///a\",\"mimeType\":"
                     "\"text/plain\",\"text\":\"alpha\\n\\\"quoted\\\"\"}],"
                     "\"ttlMs\":0,\"cacheScope\":\"private\"}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///a?bytes=5&line=2\"," META "}}");
    CONTAINS(last(), "\"uri\":\"test:///a?bytes=5&line=2\"");
    CONTAINS(last(), "\"text\":\"line=2 bytes=5\"");
    CHECK(last_line == 2 && last_bytes == 5);

    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///zzz\"," META "}}");
    CONTAINS(last(), "\"id\":5,\"error\":{\"code\":-32602,\"message\":\"no such resource\"}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///effect\"," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"reading it would act; use the read tool\"}");

    say("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"resources/read\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"uri missing\"}");

    say("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///long\"," META "}}");
    CONTAINS(last(), "xxxx\\n[cut]\"}],\"ttlMs\":0");
    LACKS(last(), "isError");

    /* A host with no resources answers the resource methods honestly. */
    reset(&host_plain, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"resources/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"resources\":[],\"ttlMs\":0");
    say("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///a\"," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"no such resource\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test:///a\"]}," META "}}");
    CONTAINS(last(), "\"notifications\":{}}}");
}

static void modern_subscriptions(void)
{
    reset(&host, sizeof out_buf, 4);
    CHECK(say("{\"jsonrpc\":\"2.0\",\"id\":\"listen-1\",\"method\":"
              "\"subscriptions/listen\",\"params\":{\"notifications\":{"
              "\"resourcesListChanged\":true,\"toolsListChanged\":true,"
              "\"resourceSubscriptions\":[\"test:///a\",\"test:///nope\","
              "\"test:///b\"]}," META "}}") == 1);
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":"
                 "\"notifications/subscriptions/acknowledged\",\"params\":{"
                 "\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":"
                 "\"listen-1\"},\"notifications\":{\"resourcesListChanged\":true,"
                 "\"resourceSubscriptions\":[\"test:///a\",\"test:///b\"]}}}");
    SAME(watched[0], "test:///a");
    SAME(watched[1], "test:///b");
    CHECK(watched[2][0] == 0);

    tinymcp_updated(&t, 0);
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/updated\","
                 "\"params\":{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":"
                 "\"listen-1\"},\"uri\":\"test:///a\"}}");
    tinymcp_updated(&t, 3);   /* an empty slot: nothing */
    CHECK(nsent == 2);
    tinymcp_listchanged(&t);
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":"
                 "\"notifications/resources/list_changed\",\"params\":{\"_meta\":"
                 "{\"io.modelcontextprotocol/subscriptionId\":\"listen-1\"}}}");

    /* A notification raised inside a tool call follows the reply. */
    nsent = 0;
    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"touch\"," META "}}");
    CHECK(nsent == 3);
    CONTAINS(sent[0], "\"id\":5,\"result\"");
    CONTAINS(sent[1], "notifications/resources/updated");
    CONTAINS(sent[2], "notifications/resources/list_changed");

    /* The client cancels: slots go, silently. */
    nsent = 0;
    CHECK(say("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\","
              "\"params\":{\"requestId\":\"listen-1\"}}") == 0);
    CHECK(unwatched[0] == 1 && unwatched[1] == 1);
    tinymcp_updated(&t, 0);
    CHECK(nsent == 0);

    /* A listen with only the list flag holds one slot and no watch. */
    say("{\"jsonrpc\":\"2.0\",\"id\":77,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourcesListChanged\":true}," META "}}");
    CONTAINS(last(), "subscriptionId\":77},\"notifications\":{\"resourcesListChanged\":true}}}");
    CHECK(watched[0][0] == 0);
    tinymcp_listchanged(&t);
    CONTAINS(last(), "list_changed\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":77}}}");

    /* A send the host refuses is counted, not retried. */
    send_fail = 1;
    tinymcp_listchanged(&t);
    CHECK(t.dropped == 1);
    send_fail = 0;

    /* Dropped on close. */
    tinymcp_closed(&t);
    nsent = 0;
    tinymcp_listchanged(&t);
    CHECK(nsent == 0);
    SAME(tinymcp_era_name(&t), "none");
}

static void modern_eviction(void)
{
    reset(&host, sizeof out_buf, 2);
    say("{\"jsonrpc\":\"2.0\",\"id\":\"L1\",\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test:///a\"]}," META "}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":\"L2\",\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test:///b\"]}," META "}}");
    nsent = 0;
    CHECK(say("{\"jsonrpc\":\"2.0\",\"id\":\"L3\",\"method\":\"subscriptions/listen\","
              "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test:///a\"]}," META "}}") == 2);
    SAME(sent[0], "{\"jsonrpc\":\"2.0\",\"id\":\"L1\",\"result\":{\"resultType\":"
                  "\"complete\",\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":"
                  "\"L1\"}}}");
    CONTAINS(sent[1], "subscriptionId\":\"L3\"},\"notifications\":{"
                      "\"resourceSubscriptions\":[\"test:///a\"]}");
    CHECK(unwatched[0] == 1);
    SAME(watched[0], "test:///a");
    SAME(watched[1], "test:///b");

    /* More URIs than slots: the first max_subs are honoured. */
    nsent = 0;
    say("{\"jsonrpc\":\"2.0\",\"id\":\"L4\",\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test:///a\","
        "\"test:///b\",\"test:///effect\"]}," META "}}");
    CHECK(nsent == 3);   /* L2 and L3 closed, then the acknowledgment */
    CONTAINS(sent[2], "\"resourceSubscriptions\":[\"test:///a\",\"test:///b\"]");

    /* A listen re-sent under the same id replaces the old one. */
    nsent = 0;
    say("{\"jsonrpc\":\"2.0\",\"id\":\"L4\",\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourceSubscriptions\":[\"test:///b\"]}," META "}}");
    CHECK(nsent == 1);
    CONTAINS(last(), "\"resourceSubscriptions\":[\"test:///b\"]");

    say("{\"jsonrpc\":\"2.0\",\"id\":\"a-listen-id-that-is-far-too-long-to-keep\","
        "\"method\":\"subscriptions/listen\",\"params\":{\"notifications\":{}," META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"id too long for a listen\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"subscriptions/listen\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"notifications missing\"}");
}

static void envelope_errors(void)
{
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,"
                 "\"message\":\"parse error\"}}");
    say("[1,2]");
    CONTAINS(last(), "\"code\":-32700");
    say("{\"jsonrpc\":\"2.0\",\"id\":2}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32600,"
                 "\"message\":\"method missing\"}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}");
    CONTAINS(last(), "\"id\":null,\"error\":{\"code\":-32600,\"message\":\"id must not be null\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\",\"params\":5}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"params must be an object\"}");
    nsent = 0;
    CHECK(say("{\"jsonrpc\":\"2.0\",\"result\":{},\"id\":4}") == 0);
    CHECK(say("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == 0);
    CHECK(say("") == 0);
    CHECK(say("   \r") == 0);

    /* A line longer than the in buffer: one parse error, then back in step. */
    {
        char big[2100];

        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = 0;
        nsent = 0;
        CHECK(say(big) == 1);
        CONTAINS(last(), "\"code\":-32700,\"message\":\"message larger than 2047 bytes\"");
        say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}");
        CONTAINS(last(), "\"id\":5,\"result\":{}}");
        /* The longest line that fits is still read whole. */
        big[sizeof in_buf - 1] = 0;
        CHECK(say(big) == 1);
        CONTAINS(last(), "\"message\":\"parse error\"");
        big[sizeof in_buf - 1] = 'a';
        big[sizeof in_buf] = 0;
        CHECK(say(big) == 1);
        CONTAINS(last(), "\"message\":\"message larger than 2047 bytes\"");
    }

    /* A frame transport hands whole messages. */
    nsent = 0;
    CHECK(tinymcp_message(&t, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"ping\"}\r\n", 42) == 0);
    CONTAINS(last(), "\"id\":6,\"result\":{}}");
    {
        char big[2100];

        memset(big, '{', sizeof big);
        CHECK(tinymcp_message(&t, big, sizeof big) == -1);
        CONTAINS(last(), "\"code\":-32700");
    }
    /* Two messages in one feed, split across calls. */
    nsent = 0;
    tinymcp_feed(&t, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"pi", 36);
    CHECK(nsent == 0);
    tinymcp_feed(&t, "ng\"}\n{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}\n", 46);
    CHECK(nsent == 2);
    CONTAINS(sent[0], "\"id\":7,");
    CONTAINS(sent[1], "\"id\":8,");
}

static void legacy_session(void)
{
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":"
        "{\"name\":\"c\",\"version\":\"1\"}}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{\"_meta\":"
                 "{\"org.tinymcp/library\":\"tinymcp " TINYMCP_VERSION "\"},"
                 "\"protocolVersion\":"
                 "\"2025-06-18\",\"capabilities\":{\"tools\":{},\"resources\":"
                 "{\"subscribe\":true,\"listChanged\":true}},\"serverInfo\":"
                 "{\"name\":\"fake\",\"version\":\"9.9\"},\"instructions\":"
                 "\"Be nice to the fake.\"}}");
    SAME(tinymcp_era_name(&t), "legacy");
    SAME(t.legacy_version, "2025-06-18");
    nsent = 0;
    CHECK(say("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == 0);

    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    CONTAINS(last(), "\"id\":2,\"result\":{\"tools\":[{\"name\":\"echo\"");
    LACKS(last(), "resultType");
    LACKS(last(), "ttlMs");

    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"add\",\"arguments\":{\"a\":1,\"b\":2}}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"content\":[{\"type\":"
                 "\"text\",\"text\":\"3\"}]}}");

    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/list\"}");
    CONTAINS(last(), "\"mimeType\":\"text/plain\"}]}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"resources/read\",\"params\":"
        "{\"uri\":\"test:///b\"}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{\"contents\":[{\"uri\":"
                 "\"test:///b\",\"mimeType\":\"text/plain\",\"text\":\"bravo\"}]}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"resources/read\",\"params\":"
        "{\"uri\":\"test:///zzz\"}}");
    CONTAINS(last(), "\"error\":{\"code\":-32002,\"message\":\"no such resource\"}");

    say("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///a\"}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}");
    SAME(watched[0], "test:///a");
    say("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///a\"}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}");
    CHECK(watched[1][0] == 0);
    tinymcp_updated(&t, 0);
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/updated\","
                 "\"params\":{\"uri\":\"test:///a\"}}");
    tinymcp_listchanged(&t);
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/resources/list_changed\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///nope\"}}");
    CONTAINS(last(), "\"error\":{\"code\":-32002");
    say("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"resources/unsubscribe\",\"params\":"
        "{\"uri\":\"test:///a\"}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":10,\"result\":{}}");
    CHECK(unwatched[0] == 1);
    nsent = 0;
    tinymcp_updated(&t, 0);
    CHECK(nsent == 0);

    /* Modern-only methods are unknown here; a modern request still works. */
    say("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"server/discover\"}");
    CONTAINS(last(), "\"id\":11,\"error\":{\"code\":-32601");
    say("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"subscriptions/listen\",\"params\":"
        "{\"notifications\":{}}}");
    CONTAINS(last(), "\"error\":{\"code\":-32601");
    say("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"ping\",\"params\":{" META "}}");
    CONTAINS(last(), "\"result\":{\"resultType\":\"complete\"");
    SAME(tinymcp_era_name(&t), "legacy");

    /* An unknown version is answered with the newest handshake one. */
    say("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2023-01-01\",\"capabilities\":{}}}");
    CONTAINS(last(), "\"protocolVersion\":\"2025-11-25\"");
    say("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"initialize\"}");
    CONTAINS(last(), "\"protocolVersion\":\"2025-11-25\"");

    /* Without watch there is nothing to subscribe to. */
    reset(&host_plain, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\"}}");
    CONTAINS(last(), "\"capabilities\":{\"tools\":{}},");
    say("{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///a\"}}");
    CONTAINS(last(), "\"error\":{\"code\":-32601");
}

static void legacy_eviction(void)
{
    reset(&host, sizeof out_buf, 1);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\"}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///a\"}}");
    nsent = 0;
    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///b\"}}");
    CHECK(nsent == 1);
    CONTAINS(last(), "\"id\":2,\"result\":{}}");
    CHECK(unwatched[0] == 1);
    SAME(watched[0], "test:///b");
    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/subscribe\",\"params\":"
        "{\"uri\":\"test:///aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}");
    CONTAINS(last(), "\"error\":{\"code\":-32602,\"message\":\"uri too long\"}");
}

static void json_edges(void)
{
    tinymcp_jv_t v, k, item;
    char s[64];
    size_t pos = 0;

    CHECK(tinymcp_json_value("  {\"a\":[1,{\"b\":\"]}\"}],\"c\":true}", 31, &v) == 31);
    CHECK(v.type == TINYMCP_J_OBJECT && v.len == 29);
    CHECK(tinymcp_json_get(&v, "c", &k) == 1 && k.type == TINYMCP_J_TRUE);
    CHECK(tinymcp_json_get(&v, "zz", &k) == 0);
    CHECK(tinymcp_json_get(&v, "a", &k) == 1 && k.type == TINYMCP_J_ARRAY);
    CHECK(tinymcp_json_item(&k, &pos, &item) == 1 && item.type == TINYMCP_J_NUMBER);
    CHECK(tinymcp_json_item(&k, &pos, &item) == 1 && item.type == TINYMCP_J_OBJECT);
    CHECK(tinymcp_json_item(&k, &pos, &item) == 0);
    CHECK(tinymcp_json_value("{\"a\":1", 6, &v) == 0);
    CHECK(tinymcp_json_value("\"open", 5, &v) == 0);
    CHECK(tinymcp_json_value("tru", 3, &v) == 0);
    CHECK(tinymcp_json_value("-12.5e3,", 8, &v) == 7 && v.type == TINYMCP_J_NUMBER);
    CHECK(tinymcp_json_long(&v) == -12);
    CHECK(tinymcp_json_value("{\"a\" 1}", 7, &v) == 7);
    CHECK(tinymcp_json_get(&v, "a", &k) == -1);
    CHECK(tinymcp_json_value("{,}", 3, &v) == 3 && tinymcp_json_get(&v, "a", &k) == -1);

    strcpy(s, "a\\u0041\\ud83d\\ude00\\ud83d-\\udc00\\/\\b\\f\\u00ZZ");
    CHECK(tinymcp_json_decode(s, 7) == 2 && !strcmp(s, "aA"));
    strcpy(s, "\\ud83d\\ude00|\\ud83d-|\\udc00|\\/\\b\\f");
    CHECK(tinymcp_json_decode(s, strlen(s)) == 17);
    CHECK(!memcmp(s, "\xf0\x9f\x98\x80|\xef\xbf\xbd-|\xef\xbf\xbd|/\b\f", 17));
    strcpy(s, "\\u00ZZ");
    CHECK(tinymcp_json_decode(s, 6) == -1);
    strcpy(s, "\\q");
    CHECK(tinymcp_json_decode(s, 2) == -1);
    strcpy(s, "\\");
    CHECK(tinymcp_json_decode(s, 1) == -1);
    strcpy(s, "\\u12");
    CHECK(tinymcp_json_decode(s, 4) == -1);

    {
        tinymcp_jw_t w;
        char buf[16];

        tinymcp_jw_init(&w, buf, sizeof buf);
        tinymcp_jw_raw(&w, "{");
        tinymcp_jw_key(&w, "k");
        tinymcp_jw_long(&w, -5);
        tinymcp_jw_raw(&w, "}");
        SAME(buf, "{\"k\":-5}");
        CHECK(!w.full);
        tinymcp_jw_str(&w, "too long to fit here");
        CHECK(w.full);
        SAME(buf, "{\"k\":-5}");
        tinymcp_jw_reset(&w, 1);
        SAME(buf, "{");
        CHECK(!w.full);
        tinymcp_jw_str(&w, "\x01\x1f");
        SAME(buf, "{\"\\u0001\\u001f\"");
        tinymcp_jw_init(&w, buf, sizeof buf);
        tinymcp_jw_long(&w, -2147483647L - 1);
        SAME(buf, "-2147483648");
    }
    {
        /* The terminator always has its byte. */
        tinymcp_jw_t w;
        char tiny[4];

        tinymcp_jw_init(&w, tiny, sizeof tiny);
        tinymcp_jw_raw(&w, "abc");
        CHECK(!w.full && !strcmp(tiny, "abc"));
        tinymcp_jw_init(&w, tiny, sizeof tiny);
        tinymcp_jw_raw(&w, "abcd");
        CHECK(w.full && tiny[0] == 0);
    }
}

static void modern_identity(void)
{
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"io.modelcontextprotocol/serverInfo\":{\"name\":\"fake\","
                     "\"title\":\"The Fake\",\"version\":\"9.9\"}");
    CONTAINS(last(), "\"capabilities\":{\"tools\":{},\"resources\":"
                     "{\"subscribe\":true,\"listChanged\":true},\"prompts\":"
                     "{\"listChanged\":true},\"completions\":{}}");
    /* Every reply carries the title; only discover carries the library. */
    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\",\"params\":{" META "}}");
    CONTAINS(last(), "\"title\":\"The Fake\"");
    LACKS(last(), "org.tinymcp/library");

    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}");
    CONTAINS(last(), "\"serverInfo\":{\"name\":\"fake\",\"title\":\"The Fake\","
                     "\"version\":\"9.9\"}");
    CONTAINS(last(), "\"prompts\":{\"listChanged\":true},\"completions\":{}}");
}

static void modern_prompts(void)
{
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"prompts/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"prompts\":[{\"name\":\"greet\",\"description\":"
                     "\"a greeting\",\"arguments\":[{\"name\":\"who\","
                     "\"description\":\"whom to greet\",\"required\":true},"
                     "{\"name\":\"mood\",\"required\":false}]},"
                     "{\"name\":\"plain\"},{\"name\":\"huge\"}]");
    LACKS(last(), "nextCursor");
    CONTAINS(last(), "\"ttlMs\":0");

    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"greet\",\"arguments\":{\"who\":\"ada\","
        "\"mood\":\"glad\"}," META "}}");
    CONTAINS(last(), "\"messages\":[{\"role\":\"user\",\"content\":"
                     "{\"type\":\"text\",\"text\":\"Hello, ada (glad)\"}}]");
    CONTAINS(last(), "\"resultType\":\"complete\"");

    /* Arguments before the name, with an escaped quote: both still read. */
    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"prompts/get\","
        "\"params\":{\"arguments\":{\"who\":\"a\\\"b\"},\"name\":\"greet\","
        META "}}");
    CONTAINS(last(), "\"text\":\"Hello, a\\\"b\"");
    /* A value ending in a backslash and a quote shifts what follows it
     * once decoded; the name is found before that. */
    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"arguments\":{\"text\":\"q\\\\\\\"\"},\"name\":\"echo\","
        META "}}");
    CONTAINS(last(), "\"text\":\"q\\\\\\\"\"}]");
    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"prompts/get\","
        "\"params\":{\"arguments\":{\"who\":\"q\\\\\\\"\"},\"name\":\"greet\","
        META "}}");
    CONTAINS(last(), "\"text\":\"Hello, q\\\\\\\"\"}}]");

    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"greet\"," META "}}");
    CONTAINS(last(), "\"id\":5,\"error\":{\"code\":-32602,\"message\":"
                     "\"a required argument is missing\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"nobody\"," META "}}");
    CONTAINS(last(), "\"id\":6,\"error\":{\"code\":-32602,\"message\":"
                     "\"unknown prompt\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"prompts/get\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"id\":7,\"error\":{\"code\":-32602,\"message\":"
                     "\"name missing\"}");
    say("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"plain\",\"arguments\":[]," META "}}");
    CONTAINS(last(), "\"id\":8,\"error\":{\"code\":-32602");
    say("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"plain\"," META "}}");
    CONTAINS(last(), "\"text\":\"Just text.\\nTwo lines.\"}}]");
    say("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"huge\"," META "}}");
    CONTAINS(last(), "zz\\n[cut]\"}}]");
    CHECK(strlen(last()) < sizeof out_buf);

    /* Pages: a small reply holds one prompt at a time. */
    reset(&host_full, 400, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"prompts/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "{\"name\":\"greet\"");
    CONTAINS(last(), "\"nextCursor\":\"1\"");
    say("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"prompts/list\","
        "\"params\":{\"cursor\":\"1\"," META "}}");
    CONTAINS(last(), "[{\"name\":\"plain\"},{\"name\":\"huge\"}]");
    LACKS(last(), "nextCursor");
    reset(&host_full, 160, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"prompts/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"code\":-32603");

    /* A host without prompts answers neither method. */
    reset(&host_plain, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"prompts/list\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"code\":-32601");
    say("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"greet\"," META "}}");
    CONTAINS(last(), "\"code\":-32601");
    say("{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"completion/complete\","
        "\"params\":{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"greet\"},"
        "\"argument\":{\"name\":\"who\",\"value\":\"\"}," META "}}");
    CONTAINS(last(), "\"code\":-32601");
}

#define COMPLETE(id, ref, arg, extra) \
    "{\"jsonrpc\":\"2.0\",\"id\":" id ",\"method\":\"completion/complete\"," \
    "\"params\":{\"ref\":" ref ",\"argument\":" arg extra "," META "}}"

static void modern_completion(void)
{
    reset(&host_full, sizeof out_buf, 4);
    say(COMPLETE("1", "{\"type\":\"ref/prompt\",\"name\":\"greet\"}",
                 "{\"name\":\"who\",\"value\":\"a\"}", ""));
    CONTAINS(last(), "\"completion\":{\"values\":[\"ada\",\"alan\"],"
                     "\"total\":2,\"hasMore\":false}");
    CONTAINS(last(), "\"resultType\":\"complete\"");
    /* The value's escapes are decoded before matching. */
    say(COMPLETE("2", "{\"type\":\"ref/prompt\",\"name\":\"greet\"}",
                 "{\"name\":\"who\",\"value\":\"a\\u0064\"}", ""));
    CONTAINS(last(), "\"values\":[\"ada\"],\"total\":1,\"hasMore\":false}");
    say(COMPLETE("3", "{\"type\":\"ref/prompt\",\"name\":\"greet\"}",
                 "{\"name\":\"mood\",\"value\":\"\"}", ""));
    CONTAINS(last(), "\"values\":[],\"total\":0,\"hasMore\":false}");

    /* A hundred values at most; the total says how many there were. */
    say(COMPLETE("4", "{\"type\":\"ref/resource\",\"uri\":\"test:///{+path}\"}",
                 "{\"name\":\"path\",\"value\":\"p\"}", ""));
    CONTAINS(last(), "\"p099\"],\"total\":150,\"hasMore\":true}");
    LACKS(last(), "p100");
    say(COMPLETE("5", "{\"type\":\"ref/resource\",\"uri\":\"test:///{+path}\"}",
                 "{\"name\":\"path\",\"value\":\"p14\"}", ""));
    CONTAINS(last(), "\"values\":[\"p140\",");
    CONTAINS(last(), "\"p149\"],\"total\":10,\"hasMore\":false}");
    /* The context's arguments reach the host. */
    say(COMPLETE("6", "{\"type\":\"ref/resource\",\"uri\":\"test:///{+path}\"}",
                 "{\"name\":\"path\",\"value\":\"\"}",
                 ",\"context\":{\"arguments\":{\"dir\":\"d\\/x\"}}"));
    CONTAINS(last(), "\"values\":[\"d/x\"],\"total\":1");

    say(COMPLETE("7", "{\"type\":\"ref/prompt\",\"name\":\"nobody\"}",
                 "{\"name\":\"who\",\"value\":\"\"}", ""));
    CONTAINS(last(), "\"id\":7,\"error\":{\"code\":-32602,\"message\":"
                     "\"unknown prompt\"}");
    say(COMPLETE("8", "{\"type\":\"ref/resource\",\"uri\":\"test:///x\"}",
                 "{\"name\":\"path\",\"value\":\"\"}", ""));
    CONTAINS(last(), "\"message\":\"unknown template\"");
    say(COMPLETE("9", "{\"type\":\"ref/other\",\"name\":\"greet\"}",
                 "{\"name\":\"who\",\"value\":\"\"}", ""));
    CONTAINS(last(), "\"id\":9,\"error\":{\"code\":-32602");
    say(COMPLETE("10", "{\"type\":\"ref/prompt\",\"name\":\"greet\"}",
                 "{\"name\":\"who\"}", ""));
    CONTAINS(last(), "\"id\":10,\"error\":{\"code\":-32602");
    say(COMPLETE("11", "{\"type\":\"ref/prompt\",\"name\":7}",
                 "{\"name\":\"who\",\"value\":\"\"}", ""));
    CONTAINS(last(), "\"id\":11,\"error\":{\"code\":-32602");
    say("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"completion/complete\","
        "\"params\":{" META "}}");
    CONTAINS(last(), "\"id\":12,\"error\":{\"code\":-32602");

    /* A small reply keeps the values that fit and says there are more. */
    reset(&host_full, 400, 4);
    say(COMPLETE("13", "{\"type\":\"ref/resource\",\"uri\":\"test:///{+path}\"}",
                 "{\"name\":\"path\",\"value\":\"\"}", ""));
    CONTAINS(last(), "\"total\":150,\"hasMore\":true}");
    CHECK(strlen(last()) < 400);

    /* The handshake era completes too. */
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"completion/complete\","
        "\"params\":{\"ref\":{\"type\":\"ref/prompt\",\"name\":\"greet\"},"
        "\"argument\":{\"name\":\"who\",\"value\":\"g\"}}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":14,\"result\":{\"completion\":"
                 "{\"values\":[\"grace\"],\"total\":1,\"hasMore\":false}}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"prompts/get\","
        "\"params\":{\"name\":\"greet\",\"arguments\":{\"who\":\"bo\"}}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":15,\"result\":{\"messages\":"
                 "[{\"role\":\"user\",\"content\":{\"type\":\"text\","
                 "\"text\":\"Hello, bo\"}}]}}");
}

static void prompts_changed(void)
{
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":\"L\",\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"promptsListChanged\":true}," META "}}");
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":"
                 "\"notifications/subscriptions/acknowledged\",\"params\":"
                 "{\"_meta\":{\"io.modelcontextprotocol/subscriptionId\":\"L\"},"
                 "\"notifications\":{\"promptsListChanged\":true}}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":\"R\",\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourcesListChanged\":true}," META "}}");
    nsent = 0;
    tinymcp_prompts_changed(&t);
    CHECK(nsent == 1);
    SAME(sent[0], "{\"jsonrpc\":\"2.0\",\"method\":"
                  "\"notifications/prompts/list_changed\",\"params\":{\"_meta\":"
                  "{\"io.modelcontextprotocol/subscriptionId\":\"L\"}}}");
    nsent = 0;
    tinymcp_listchanged(&t);
    CHECK(nsent == 1);
    CONTAINS(sent[0], "notifications/resources/list_changed");
    CONTAINS(sent[0], "\"R\"");

    /* From inside a call the notice follows the reply. */
    nsent = 0;
    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"reprompt\"," META "}}");
    CHECK(nsent == 2);
    CONTAINS(sent[0], "\"id\":3,\"result\"");
    CONTAINS(sent[1], "notifications/prompts/list_changed");

    /* One listen asks for both; a host without prompts drops the ask. */
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"resourcesListChanged\":true,"
        "\"promptsListChanged\":true}," META "}}");
    CONTAINS(last(), "{\"resourcesListChanged\":true,\"promptsListChanged\":true}");
    nsent = 0;
    tinymcp_prompts_changed(&t);
    tinymcp_listchanged(&t);
    CHECK(nsent == 2);
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"subscriptions/listen\","
        "\"params\":{\"notifications\":{\"promptsListChanged\":true}," META "}}");
    CONTAINS(last(), "\"notifications\":{}}}");
    nsent = 0;
    tinymcp_prompts_changed(&t);
    CHECK(nsent == 0);

    /* The handshake era hears it without a listen. */
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}");
    tinymcp_prompts_changed(&t);
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"method\":"
                 "\"notifications/prompts/list_changed\"}");
}

static void prompt_templates(void)
{
    static const char tpl[] = "Hi {{who}}, {{who}} again; {{ x }} {{} "
                              "{{lang_2}} {{a-b}} {{tail";
    char names[4][TINYMCP_PNAME_MAX], buf[256], long_name[40];
    tinymcp_jw_t w;
    tinymcp_sink_t sink;
    tinymcp_args_t args;

    CHECK(tinymcp_placeholders(tpl, strlen(tpl), names, 4) == 2);
    SAME(names[0], "who");
    SAME(names[1], "lang_2");
    CHECK(tinymcp_placeholders(tpl, strlen(tpl), names, 1) == 1);
    CHECK(tinymcp_placeholders("{{x}}", 4, names, 4) == 0);
    CHECK(tinymcp_placeholders("{{x}}", 5, names, 4) == 1);
    /* A name of 31 characters is one; 32 is text. */
    memset(long_name, 'n', sizeof long_name);
    memcpy(long_name, "{{", 2);
    memcpy(long_name + 33, "}}", 2);
    CHECK(tinymcp_placeholders(long_name, 35, names, 4) == 1);
    long_name[33] = 'n';
    memcpy(long_name + 34, "}}", 2);
    CHECK(tinymcp_placeholders(long_name, 36, names, 4) == 0);

    memset(&args, 0, sizeof args);
    args.n = 2;
    args.a[0].key = "who";
    args.a[0].str = "Ada";
    args.a[1].key = "lang_2";
    args.a[1].str = "C\"";
    tinymcp_jw_init(&w, buf, sizeof buf);
    tinymcp_sink_init(&sink, &w, sizeof buf);
    CHECK(tinymcp_render(&sink, tpl, strlen(tpl), &args) == 0);
    SAME(buf, "Hi Ada, Ada again; {{ x }} {{} C\\\" {{a-b}} {{tail");

    /* A placeholder without its argument writes nothing. */
    args.n = 1;
    tinymcp_jw_init(&w, buf, sizeof buf);
    tinymcp_sink_init(&sink, &w, sizeof buf);
    CHECK(tinymcp_render(&sink, tpl, strlen(tpl), &args) == TINYMCP_EINVAL);
    CHECK(w.len == 0);
    /* A number is not a string argument. */
    args.n = 2;
    args.a[1].str = NULL;
    CHECK(tinymcp_render(&sink, tpl, strlen(tpl), &args) == TINYMCP_EINVAL);
}

#define META_WHO(who) "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":" \
    "\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":{}," \
    "\"io.modelcontextprotocol/clientInfo\":{\"name\":" who ",\"version\":\"1\"}}"

static void client_names(void)
{
    reset(&host, sizeof out_buf, 4);
    SAME(tinymcp_client(&t), "");
    /* The current revision: each request names its client in _meta, and the
     * params around it still read after the name is kept. */
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{"
        "\"name\":\"echo\",\"arguments\":{\"text\":\"hi\"},"
        META_WHO("\"Claude \\\"Code\\\" \\u00e9\"") "}}");
    SAME(tinymcp_client(&t), "Claude \"Code\" \xc3\xa9");
    CONTAINS(last(), "\"text\":\"hi\"}]");
    say("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\",\"params\":{" META "}}");
    SAME(tinymcp_client(&t), "");
    /* A long name is cut, and never inside an escape. */
    say("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\",\"params\":{"
        META_WHO("\"0123456789012345678901234567890123456789\"") "}}");
    SAME(tinymcp_client(&t), "0123456789012345678901234567890");
    say("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\",\"params\":{"
        META_WHO("\"012345678901234567890123456\\u00e9xx\"") "}}");
    SAME(tinymcp_client(&t), "012345678901234567890123456");
    say("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\",\"params\":{"
        META_WHO("7") "}}");
    SAME(tinymcp_client(&t), "");

    /* The handshake era: initialize names the client for the session. */
    reset(&host, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"old-client\",\"version\":\"1\"}}}");
    SAME(tinymcp_client(&t), "old-client");
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    SAME(tinymcp_client(&t), "old-client");
    tinymcp_closed(&t);
    SAME(tinymcp_client(&t), "");
}

#define META_EL "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":" \
    "\"2026-07-28\",\"io.modelcontextprotocol/clientCapabilities\":" \
    "{\"elicitation\":{\"form\":{}}}}"
#define GUARDED(id, extra, meta) \
    "{\"jsonrpc\":\"2.0\",\"id\":" id ",\"method\":\"tools/call\"," \
    "\"params\":{\"name\":\"guarded\"" extra "," meta "}}"
#define ACCEPT(v) ",\"inputResponses\":{\"confirm\":{\"action\":\"accept\"," \
    "\"content\":{\"confirm\":" v "}}}"

static void consent(void)
{
    char buf[4];

    reset(&host_full, sizeof out_buf, 4);
    say(GUARDED("1", "", META_EL));
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"resultType\":"
                 "\"input_required\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":"
                 "{\"name\":\"fake\",\"title\":\"The Fake\",\"version\":\"9.9\"}},"
                 "\"inputRequests\":{\"confirm\":{\"method\":\"elicitation/create\","
                 "\"params\":{\"mode\":\"form\",\"message\":\"Really do it?\","
                 "\"requestedSchema\":" CONFIRM_SCHEMA "}}},\"requestState\":\"s1\"}}");
    /* Between requests nothing points into the message just served. */
    CHECK(t.el_key == NULL && t.el_state == NULL);
    /* The retry with a yes and the state does it. */
    say(GUARDED("2", ACCEPT("true") ",\"requestState\":\"s\\u0031\"", META_EL));
    CONTAINS(last(), "\"id\":2,\"result\":{\"resultType\":\"complete\"");
    CONTAINS(last(), "\"text\":\"done\"}]");
    LACKS(last(), "isError");
    /* A yes without its state, or with confirm false, is asked again. */
    say(GUARDED("3", ACCEPT("true"), META_EL));
    CONTAINS(last(), "\"resultType\":\"input_required\"");
    say(GUARDED("4", ACCEPT("false") ",\"requestState\":\"s1\"", META_EL));
    CONTAINS(last(), "\"resultType\":\"input_required\"");
    say(GUARDED("5", ",\"inputResponses\":{\"confirm\":{\"action\":\"decline\"}},"
                "\"requestState\":\"s1\"", META_EL));
    CONTAINS(last(), "\"text\":\"declined\"}],\"isError\":true");
    CHECK(t.in_responses.type == TINYMCP_J_NONE &&
          t.in_state.type == TINYMCP_J_NONE &&
          tinymcp_input_action(&t, "confirm") == TINYMCP_INPUT_NONE);
    say(GUARDED("6", ",\"inputResponses\":{\"confirm\":{\"action\":\"cancel\"}}",
                META_EL));
    CONTAINS(last(), "\"text\":\"cancelled\"}],\"isError\":true");
    /* The next request carries none of it: no answers, no form. */
    say(GUARDED("7", "", META));
    CONTAINS(last(), "\"text\":\"cannot ask\"}],\"isError\":true");
    CHECK(t.el_key == NULL && t.in_responses.type == TINYMCP_J_NONE &&
          t.in_state.type == TINYMCP_J_NONE && !t.elicits);
    CHECK(tinymcp_input_action(&t, "confirm") == TINYMCP_INPUT_NONE);
    CHECK(tinymcp_input_bool(&t, "confirm", "confirm") == -1);
    CHECK(tinymcp_request_state(&t, buf, sizeof buf) == -1);
    /* A read can ask too; a host that asks without a form is an error. */
    say("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/read\","
        "\"params\":{\"uri\":\"test:///guarded\"," META_EL "}}");
    CONTAINS(last(), "\"id\":8,\"result\":{\"resultType\":\"input_required\"");
    say("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"lazy\"," META_EL "}}");
    CONTAINS(last(), "\"id\":9,\"error\":{\"code\":-32603");
    /* A state longer than the buffer is refused, not cut. */
    say("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\",\"params\":{"
        "\"requestState\":\"abcd\"," META_EL "}}");
    CHECK(tinymcp_client_elicits(&t) == 0);
    /* The handshake era cannot carry input_required. */
    reset(&host_full, sizeof out_buf, 4);
    say("{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"elicitation\":{}}}}");
    say("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"guarded\"}}");
    CONTAINS(last(), "\"text\":\"cannot ask\"}],\"isError\":true");
    CHECK(tinymcp_elicit(&t, "k", "m", "{}", NULL) == -1);
}

/* The accessors on one message, held mid-request. */
static void consent_accessors(void)
{
    char msg[] = "{\"confirm\":{\"action\":\"accept\",\"content\":"
                 "{\"confirm\":true,\"n\":3}},\"x\":{\"action\":\"maybe\"}}";
    char state[] = "\"abc\"";
    char buf[4];

    reset(&host_full, sizeof out_buf, 4);
    tinymcp_json_value(msg, strlen(msg), &t.in_responses);
    tinymcp_json_value(state, strlen(state), &t.in_state);
    CHECK(tinymcp_input_action(&t, "confirm") == TINYMCP_INPUT_ACCEPT);
    CHECK(tinymcp_input_action(&t, "x") == TINYMCP_INPUT_NONE);
    CHECK(tinymcp_input_action(&t, "nope") == TINYMCP_INPUT_NONE);
    CHECK(tinymcp_input_bool(&t, "confirm", "confirm") == 1);
    CHECK(tinymcp_input_bool(&t, "confirm", "n") == -1);
    CHECK(tinymcp_input_bool(&t, "x", "confirm") == -1);
    CHECK(tinymcp_request_state(&t, buf, sizeof buf) == 3 && !strcmp(buf, "abc"));
    CHECK(tinymcp_request_state(&t, buf, 3) == -1);
    t.in_responses.type = TINYMCP_J_NONE;
    t.in_state.type = TINYMCP_J_NONE;
}

int main(void)
{
    json_edges();
    modern_discover();
    modern_meta_rules();
    modern_tools();
    modern_paging();
    modern_resources();
    modern_subscriptions();
    modern_eviction();
    envelope_errors();
    legacy_session();
    legacy_eviction();
    modern_identity();
    modern_prompts();
    modern_completion();
    prompts_changed();
    prompt_templates();
    client_names();
    consent();
    consent_accessors();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
