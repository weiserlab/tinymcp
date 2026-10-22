/*
 * tinymcp v0.1
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * test_tinymcp.c - the protocol against a fake host: both eras, every
 * method, every error code, the buffers' edges, subscriptions and their
 * eviction.  Each check is known to fail on a mutant (tests/mutants.sh).
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
};
#define NTOOLS (sizeof tools / sizeof tools[0])

static const tinymcp_template_t templates[] = {
    { "test:///{+path}", "a path", "any path" },
    { "test:///{+path}{?line,bytes}", "a page", NULL },
};

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

static const tinymcp_host_t host = {
    "fake", "9.9", "Be nice to the fake.",
    tools, NTOOLS, templates, 2,
    fake_list, fake_read, fake_tool, fake_watch, fake_unwatch, fake_send
};

static const tinymcp_host_t host_plain = {
    "plain", "1", NULL,
    tools, 1, NULL, 0,
    NULL, NULL, fake_tool, NULL, NULL, fake_send
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
                     "{\"name\":\"fake\",\"version\":\"9.9\"}}");
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
    SAME(last(), "{\"jsonrpc\":\"2.0\",\"id\":0,\"result\":{\"protocolVersion\":"
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
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
