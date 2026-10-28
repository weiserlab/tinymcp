/*
 * tinymcp v0.8
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp_posix.c - an MCP server over stdin and stdout, or Streamable HTTP
 * on a local port, that serves one directory's files as resources and
 * prompts, runs lines through the shell, counts slowly in a call it answers
 * later, and streams what does not fit: the conformance target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tinymcp.h"
#include "tinymcp_http.h"

#define IN_CAP  16384
#define OUT_CAP 32768
#define NAME_MAX_LEN 128

#define CONNS 4   /* conns[0] is stdio, or HTTP's connections from 1 */

/* One stream: stdio's, or one HTTP connection's. */
static struct conn {
    tinymcp_t t;
    tinymcp_http_t h;
    char in[IN_CAP], out[OUT_CAP];
    char watched[TINYMCP_SUBS_MAX][NAME_MAX_LEN];
    char pend[4096];     /* bytes read past a request whose response is owed */
    size_t npend;
    int fd;              /* -1 for stdio */
    int used;
} conns[CONNS];

static tinymcp_t *cur;   /* the instance serving the request now */
static const char *dir = ".";
static int trace;

static struct conn *conn_of(const tinymcp_t *t)
{
    return (struct conn *)((char *)t - offsetof(struct conn, t));
}

static const char *const uri_prefix = "file:///";
static const char *const prompt_prefix = "prompt.";
#define PROMPT_MAX 8192   /* the longest prompt file served */

static int is_prompt_file(const char *name)
{
    return !strncmp(name, prompt_prefix, strlen(prompt_prefix)) &&
           name[strlen(prompt_prefix)];
}

/* The file name a URI names, or NULL when it is not one of this directory. */
static const char *uri_name(const char *uri)
{
    const char *n;

    if (strncmp(uri, uri_prefix, strlen(uri_prefix)))
        return NULL;
    n = uri + strlen(uri_prefix);
    if (!*n || strchr(n, '/') || !strcmp(n, "..") ||
        strlen(n) >= NAME_MAX_LEN)
        return NULL;
    return n;
}

static int path_of(const char *name, char *out, size_t cap)
{
    if (snprintf(out, cap, "%s/%s", dir, name) >= (int)cap)
        return -1;
    return 0;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The directory's regular files, sorted: the prompt files when prompts is
 * set, the others when not.  The caller frees each name. */
static unsigned files(char *names[], unsigned max, int prompts)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    unsigned n = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        char path[512];
        struct stat st;

        if (e->d_name[0] == '.' || path_of(e->d_name, path, sizeof path))
            continue;
        if (stat(path, &st) || !S_ISREG(st.st_mode))
            continue;
        if (is_prompt_file(e->d_name) != prompts)
            continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], cmp_names);
    return n;
}

/* The directory's files but the prompts, as resources from cursor on. */
static int list(void *ctx, unsigned cursor, tinymcp_list_t *out)
{
    char *names[256];
    unsigned n, i;
    int next = 0;

    (void)ctx;
    n = files(names, 256, 0);
    for (i = cursor; i < n && !next; i++) {
        char uri[NAME_MAX_LEN + 16];

        snprintf(uri, sizeof uri, "%s%s", uri_prefix, names[i]);
        if (tinymcp_list_add(out, uri, names[i], NULL) != 0)
            next = i ? (int)i : TINYMCP_EFULL;
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    return next;
}

/* The file's text, from line on, at most bytes of it when bytes is set. */
/* The MIME type of a file whose first 512 bytes hold a NUL, or NULL for
 * text: image/png by its name, application/octet-stream otherwise. */
static const char *binary_mime(FILE *f, const char *name)
{
    char head[512];
    size_t n = fread(head, 1, sizeof head, f);
    size_t k = strlen(name);

    rewind(f);
    if (!memchr(head, 0, n))
        return NULL;
    if (k > 4 && !strcmp(name + k - 4, ".png"))
        return "image/png";
    return "application/octet-stream";
}

static int read_file(void *ctx, const char *uri, unsigned line, unsigned bytes,
                     tinymcp_sink_t *text)
{
    const char *name = uri_name(uri);
    const char *mime;
    char path[512];
    FILE *f;
    int c;
    unsigned skipped = 0, written = 0;

    (void)ctx;
    if (!name || path_of(name, path, sizeof path))
        return TINYMCP_ENOENT;
    f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? TINYMCP_ENOENT : TINYMCP_EIO;
    mime = binary_mime(f, name);
    if (mime) {
        tinymcp_sink_binary(text, mime);
        line = 0;
    }
    while ((c = fgetc(f)) != EOF) {
        if (skipped < line) {
            if (c == '\n')
                skipped++;
            continue;
        }
        if (bytes && written >= bytes)
            break;
        tinymcp_sink_putc(text, (char)c);
        written++;
    }
    fclose(f);
    return TINYMCP_OK;
}

static const tinymcp_tool_t tools[] = {
    { "shell", "Run one line through /bin/sh and return what it printed.",
      "{\"type\":\"object\",\"properties\":{\"line\":{\"type\":\"string\","
      "\"description\":\"the command line\"}},\"required\":[\"line\"]}", NULL },
    { "write", "Write text to a file in the served directory, creating it.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
      "\"text\":{\"type\":\"string\"}},\"required\":[\"name\",\"text\"]}", NULL },
    { "echo", "Return the text given.",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
      "\"required\":[\"text\"]}", NULL },
    { "whoami", "Return the name the client gave in its clientInfo.",
      "{\"type\":\"object\"}", NULL },
    { "remove", "Delete a file of the served directory, after the person "
      "says yes.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
      "\"required\":[\"name\"]}", NULL },
    { "count", "Count from 1 to the number given, one every ms milliseconds "
      "(100 when absent), with progress for each; the result comes at the "
      "end.",
      "{\"type\":\"object\",\"properties\":{\"to\":{\"type\":\"integer\"},"
      "\"ms\":{\"type\":\"integer\"}},\"required\":[\"to\"]}", NULL },
    { "bearer", "Return the bearer token the HTTP request carried.",
      "{\"type\":\"object\"}", NULL },
    { "counters", "Return the counts running, finished and cancelled.",
      "{\"type\":\"object\"}", NULL },
    { "picture", "Return a file of the served directory as an image.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
      "\"required\":[\"name\"]}", NULL },
    { "stat", "Return a file's name and size as structured content.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
      "\"required\":[\"name\"]}",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
      "\"size\":{\"type\":\"integer\"}},\"required\":[\"name\",\"size\"]}" },
};

/* The counts in progress, one per kept call. */
static struct {
    tinymcp_t *t;            /* the instance the call came on */
    unsigned call;           /* 0: free */
    long to, at, ms;
    long long next;          /* when the next number is due, in ms */
} counts[TINYMCP_LATER_MAX];
static unsigned finished, cancelled;

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* count: keeps the call and counts from the main loop. */
static int count_start(const tinymcp_args_t *args, tinymcp_sink_t *text)
{
    long to = tinymcp_arg_int(args, "to", -1);
    long ms = tinymcp_arg_int(args, "ms", 100);
    unsigned i, call;

    if (to < 0 || ms < 0)
        return TINYMCP_EINVAL;
    for (i = 0; i < TINYMCP_LATER_MAX && counts[i].call; i++)
        ;
    call = i < TINYMCP_LATER_MAX ? tinymcp_defer(cur) : 0;
    if (!call) {
        tinymcp_sink_puts(text, "too many counts at once");
        return TINYMCP_TOOL_ERROR;
    }
    counts[i].t = cur;
    counts[i].call = call;
    counts[i].to = to;
    counts[i].at = 0;
    counts[i].ms = ms;
    counts[i].next = now_ms() + ms;
    return TINYMCP_LATER;
}

/* Advance every count that is due; the ms until the next one, or -1. */
static int count_tick(void)
{
    long long now = now_ms(), wait = -1;
    unsigned i;

    for (i = 0; i < TINYMCP_LATER_MAX; i++) {
        tinymcp_sink_t text;
        char n[24];

        if (!counts[i].call)
            continue;
        while (counts[i].call && counts[i].next <= now) {
            if (counts[i].at == counts[i].to) {
                if (tinymcp_result_begin(counts[i].t, counts[i].call, &text) == 0) {
                    snprintf(n, sizeof n, "counted to %ld", counts[i].to);
                    tinymcp_sink_puts(&text, n);
                    tinymcp_result_end(counts[i].t, counts[i].call, &text, 0);
                }
                counts[i].call = 0;
                finished++;
                break;
            }
            counts[i].at++;
            snprintf(n, sizeof n, "%ld", counts[i].at);
            tinymcp_progress(counts[i].t, counts[i].call, (unsigned long)counts[i].at,
                             (unsigned long)counts[i].to, n);
            counts[i].next += counts[i].ms;
        }
        if (counts[i].call && (wait < 0 || counts[i].next - now < wait))
            wait = counts[i].next - now;
    }
    return (int)wait;
}

/* The client cancelled a count: it stops without a result. */
static void cancel(void *ctx, unsigned call)
{
    struct conn *c = ctx;
    unsigned i;

    for (i = 0; i < TINYMCP_LATER_MAX; i++) {
        if (counts[i].call == call && counts[i].t == &c->t) {
            counts[i].call = 0;
            cancelled++;
        }
    }
}

#define CONFIRM_SCHEMA "{\"type\":\"object\",\"properties\":{\"confirm\":" \
    "{\"type\":\"boolean\",\"title\":\"Delete it\"}},\"required\":[\"confirm\"]}"

/* remove: asks through input_required, the file's name as the state, and
 * deletes on the retry that carries a yes for that same name. */
static int remove_file(const char *fname, tinymcp_sink_t *text)
{
    static char message[NAME_MAX_LEN + 32], state[NAME_MAX_LEN + 8];
    char back[NAME_MAX_LEN + 8], path[512];
    int action = tinymcp_input_action(cur, "confirm");

    if (strchr(fname, '/') || fname[0] == '.' || strlen(fname) >= NAME_MAX_LEN ||
        path_of(fname, path, sizeof path) || access(path, F_OK)) {
        tinymcp_sink_puts(text, "no such file");
        return TINYMCP_TOOL_ERROR;
    }
    snprintf(state, sizeof state, "rm:%s", fname);
    if (action == TINYMCP_INPUT_DECLINE || action == TINYMCP_INPUT_CANCEL) {
        tinymcp_sink_puts(text, "the person said no; nothing was deleted");
        return TINYMCP_TOOL_ERROR;
    }
    if (action == TINYMCP_INPUT_ACCEPT &&
        tinymcp_input_bool(cur, "confirm", "confirm") == 1 &&
        tinymcp_request_state(cur, back, sizeof back) >= 0 &&
        !strcmp(back, state)) {
        if (unlink(path)) {
            tinymcp_sink_puts(text, strerror(errno));
            return TINYMCP_TOOL_ERROR;
        }
        tinymcp_listchanged(cur);
        return TINYMCP_OK;
    }
    snprintf(message, sizeof message, "Delete %s?", fname);
    if (tinymcp_elicit(cur, "confirm", message, CONFIRM_SCHEMA, state)) {
        tinymcp_sink_puts(text, "deleting needs the person's yes, and this "
                                "client cannot ask");
        return TINYMCP_TOOL_ERROR;
    }
    return TINYMCP_NEED_INPUT;
}

static const tinymcp_template_t templates[] = {
    { "file:///{name}", "a file", "a file of the served directory" },
    { "file:///{name}{?line,bytes}", "a page of a file",
      "from line, at most bytes" },
};

/* A prompt file's text, NUL-terminated, into buf; its length, or -1. */
static long prompt_text(const char *pname, char *buf, size_t cap)
{
    char fname[NAME_MAX_LEN + 8], path[512];
    FILE *f;
    size_t n;

    if (strchr(pname, '/') ||
        snprintf(fname, sizeof fname, "%s%s", prompt_prefix, pname) >=
            (int)sizeof fname ||
        path_of(fname, path, sizeof path))
        return -1;
    f = fopen(path, "r");
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (long)n;
}

/* The prompts, prompt.<name> files: the first line describes each, and its
 * {{placeholders}} are its required arguments. */
static int prompt_list(void *ctx, unsigned cursor, tinymcp_list_t *out)
{
    static char text[PROMPT_MAX];
    char *names[256], desc[256];
    char ph[TINYMCP_ARGS_MAX][TINYMCP_PNAME_MAX];
    tinymcp_prompt_arg_t args[TINYMCP_ARGS_MAX];
    unsigned n, i, j, nargs;
    int next = 0;
    long len;

    (void)ctx;
    n = files(names, 256, 1);
    for (i = cursor; i < n && !next; i++) {
        const char *pname = names[i] + strlen(prompt_prefix);

        len = prompt_text(pname, text, sizeof text);
        if (len < 0)
            continue;
        snprintf(desc, sizeof desc, "%.*s", (int)strcspn(text, "\n"), text);
        nargs = tinymcp_placeholders(text, (size_t)len, ph, TINYMCP_ARGS_MAX);
        for (j = 0; j < nargs; j++) {
            args[j].name = ph[j];
            args[j].description = NULL;
            args[j].required = 1;
        }
        if (tinymcp_prompt_add(out, pname, desc[0] ? desc : NULL, args,
                               nargs) != 0)
            next = i ? (int)i : TINYMCP_EFULL;
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    return next;
}

static int prompt_get(void *ctx, const char *pname, const tinymcp_args_t *args,
                      tinymcp_sink_t *text)
{
    static char body[PROMPT_MAX];
    long len;

    (void)ctx;
    len = prompt_text(pname, body, sizeof body);
    if (len < 0)
        return TINYMCP_ENOENT;
    return tinymcp_render(text, body, (size_t)len, args);
}

/* File names for the templates' name and a prompt argument called name. */
static int complete(void *ctx, int is_prompt, const char *ref, const char *arg,
                    const char *value, const tinymcp_args_t *context,
                    tinymcp_completion_t *c)
{
    char *names[256];
    unsigned n, i;

    (void)ctx;
    (void)context;
    if (is_prompt) {
        char probe[8];

        if (prompt_text(ref, probe, sizeof probe) < 0)
            return TINYMCP_ENOENT;
    } else if (strcmp(ref, "file:///{name}") &&
               strcmp(ref, "file:///{name}{?line,bytes}")) {
        return TINYMCP_ENOENT;
    }
    if (strcmp(arg, "name"))
        return TINYMCP_OK;
    n = files(names, 256, 0);
    for (i = 0; i < n; i++) {
        if (!strncmp(names[i], value, strlen(value)))
            tinymcp_complete_add(c, names[i]);
        free(names[i]);
    }
    return TINYMCP_OK;
}

static int call(void *ctx, const char *name, const tinymcp_args_t *args,
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
    if (!strcmp(name, "remove")) {
        const char *fname = tinymcp_arg_str(args, "name");

        if (!fname)
            return TINYMCP_EINVAL;
        return remove_file(fname, text);
    }
    if (!strcmp(name, "count"))
        return count_start(args, text);
    if (!strcmp(name, "picture") || !strcmp(name, "stat")) {
        const char *fname = tinymcp_arg_str(args, "name");
        static char json[NAME_MAX_LEN + 64];
        char path[512];
        struct stat st;
        FILE *f;
        int c;

        if (!fname || strpbrk(fname, "/\"\\") ||
            path_of(fname, path, sizeof path) ||
            stat(path, &st) != 0) {
            tinymcp_sink_puts(text, "no such file");
            return TINYMCP_TOOL_ERROR;
        }
        if (!strcmp(name, "stat")) {
            snprintf(json, sizeof json, "{\"name\":\"%s\",\"size\":%ld}",
                     fname, (long)st.st_size);
            tinymcp_sink_puts(text, json);
            tinymcp_structured(cur, json);
            return TINYMCP_OK;
        }
        f = fopen(path, "rb");
        if (!f || tinymcp_sink_binary(text, "image/png") != 0) {
            if (f)
                fclose(f);
            return TINYMCP_EIO;
        }
        while ((c = fgetc(f)) != EOF)
            tinymcp_sink_putc(text, (char)c);
        fclose(f);
        return TINYMCP_OK;
    }
    if (!strcmp(name, "counters")) {
        char line[64];
        unsigned i, running = 0;

        for (i = 0; i < TINYMCP_LATER_MAX; i++)
            if (counts[i].call)
                running++;
        snprintf(line, sizeof line, "running %u finished %u cancelled %u",
                 running, finished, cancelled);
        tinymcp_sink_puts(text, line);
        return TINYMCP_OK;
    }
    if (!strcmp(name, "bearer")) {
        struct conn *c = conn_of(cur);

        tinymcp_sink_puts(text, c->fd >= 0 ? tinymcp_http_bearer(&c->h) : "");
        return TINYMCP_OK;
    }
    if (!strcmp(name, "whoami")) {
        tinymcp_sink_puts(text, tinymcp_client(cur));
        return TINYMCP_OK;
    }
    if (!strcmp(name, "shell")) {
        const char *line = tinymcp_arg_str(args, "line");
        FILE *p;
        int c, status;

        if (!line)
            return TINYMCP_EINVAL;
        p = popen(line, "r");
        if (!p) {
            tinymcp_sink_puts(text, strerror(errno));
            return TINYMCP_TOOL_ERROR;
        }
        while ((c = fgetc(p)) != EOF)
            tinymcp_sink_putc(text, (char)c);
        status = pclose(p);
        return status == 0 ? TINYMCP_OK : TINYMCP_TOOL_ERROR;
    }
    if (!strcmp(name, "write")) {
        const char *fname = tinymcp_arg_str(args, "name");
        const char *body = tinymcp_arg_str(args, "text");
        char path[512];
        FILE *f;
        int existed, k;
        unsigned i;

        if (!fname || !body)
            return TINYMCP_EINVAL;
        if (strchr(fname, '/') || fname[0] == '.' ||
            strlen(fname) >= NAME_MAX_LEN || path_of(fname, path, sizeof path)) {
            tinymcp_sink_puts(text, "name must be a plain file name");
            return TINYMCP_TOOL_ERROR;
        }
        existed = access(path, F_OK) == 0;
        f = fopen(path, "w");
        if (!f) {
            tinymcp_sink_puts(text, strerror(errno));
            return TINYMCP_TOOL_ERROR;
        }
        fputs(body, f);
        fclose(f);
        /* Subscribers hear of it after this reply has gone out. */
        for (k = 0; k < CONNS; k++) {
            if (!conns[k].used)
                continue;
            for (i = 0; i < TINYMCP_SUBS_MAX; i++)
                if (!strcmp(conns[k].watched[i], fname))
                    tinymcp_updated(&conns[k].t, i);
            if (is_prompt_file(fname))
                tinymcp_prompts_changed(&conns[k].t);
            else if (!existed)
                tinymcp_listchanged(&conns[k].t);
        }
        return TINYMCP_OK;
    }
    return TINYMCP_EINVAL;
}

static int watch(void *ctx, const char *uri, unsigned slot)
{
    struct conn *c = ctx;
    const char *name = uri_name(uri);
    char path[512];

    if (!name || path_of(name, path, sizeof path) || access(path, F_OK))
        return -1;
    strncpy(c->watched[slot], name, NAME_MAX_LEN - 1);
    c->watched[slot][NAME_MAX_LEN - 1] = 0;
    return 0;
}

static void unwatch(void *ctx, unsigned slot)
{
    struct conn *c = ctx;

    c->watched[slot][0] = 0;
}

static int send_line(void *ctx, const char *msg, size_t len)
{
    struct conn *c = ctx;

    if (c->fd >= 0)
        return tinymcp_http_send(&c->h, msg, len);
    if (trace)
        fprintf(stderr, "<- %s\n", msg);
    if (fwrite(msg, 1, len, stdout) != len || fputc('\n', stdout) == EOF)
        return -1;
    fflush(stdout);
    return 0;
}

/* A message in pieces: each piece as it comes, the newline after the last. */
static int send_piece(void *ctx, const char *bytes, size_t len, int last)
{
    struct conn *c = ctx;

    if (c->fd >= 0)
        return tinymcp_http_send_part(&c->h, bytes, len, last);
    if (trace)
        fprintf(stderr, "<- %.*s%s", (int)len, bytes, last ? "\n" : "");
    if (fwrite(bytes, 1, len, stdout) != len ||
        (last && fputc('\n', stdout) == EOF))
        return -1;
    if (last)
        fflush(stdout);
    return 0;
}

static const tinymcp_host_t host = {
    "tinymcp-posix", TINYMCP_VERSION,
    "The files of one directory are resources; the shell tool runs a line "
    "where the server runs.",
    tools, sizeof tools / sizeof tools[0],
    templates, sizeof templates / sizeof templates[0],
    list, read_file, call, watch, unwatch, send_line,
    "tinymcp on POSIX", prompt_list, prompt_get, complete, cancel, send_piece
};

/* An HTTP connection's bytes, sent whole; the end-of-response mark is
 * nothing to a socket. */
static int conn_write(void *ctx, const char *bytes, size_t n)
{
    struct conn *c = ctx;

    while (bytes && n) {
        ssize_t k = send(c->fd, bytes, n, 0);

        if (k <= 0)
            return -1;
        bytes += k;
        n -= (size_t)k;
    }
    return 0;
}

static void conn_open(struct conn *c, int fd)
{
    memset(c->watched, 0, sizeof c->watched);
    c->fd = fd;
    c->npend = 0;
    c->used = 1;
    tinymcp_init(&c->t, &host, c, c->in, sizeof c->in, c->out, sizeof c->out,
                 TINYMCP_SUBS_MAX);
    if (fd >= 0)
        tinymcp_http_init(&c->h, &c->t, "/mcp", conn_write, c);
}

/* Its kept counts and listens end with it. */
static void conn_close(struct conn *c)
{
    cur = &c->t;
    tinymcp_closed(&c->t);
    if (c->fd >= 0)
        close(c->fd);
    c->used = 0;
}

/* Hand the connection the bytes it holds, as far as it takes them. */
static void conn_drain(struct conn *c)
{
    size_t taken;

    cur = &c->t;
    taken = tinymcp_http_feed(&c->h, c->pend, c->npend);
    memmove(c->pend, c->pend + taken, c->npend - taken);
    c->npend -= taken;
    if (c->h.state == TINYMCP_HTTP_S_CLOSE)
        conn_close(c);
}

/* Streamable HTTP on 127.0.0.1:port, POST /mcp, until killed. */
static int serve_http(int port)
{
    struct sockaddr_in a;
    int one = 1, ls = socket(AF_INET, SOCK_STREAM, 0);
    socklen_t alen = sizeof a;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001UL);   /* 127.0.0.1 */
    a.sin_port = htons((unsigned short)port);
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8) ||
        getsockname(ls, (struct sockaddr *)&a, &alen)) {
        perror("tinymcp_posix: http");
        return 1;
    }
    fprintf(stderr, "listening %d\n", ntohs(a.sin_port));
    fflush(stderr);
    for (;;) {
        struct pollfd p[CONNS];
        int i;

        p[0].fd = ls;
        p[0].events = POLLIN;
        p[0].revents = 0;
        for (i = 1; i < CONNS; i++) {
            p[i].fd = conns[i].used ? conns[i].fd : -1;
            p[i].events = POLLIN;
            p[i].revents = 0;
        }
        if (poll(p, CONNS, count_tick()) > 0) {
            if (p[0].revents & POLLIN) {
                int fd = accept(ls, NULL, NULL);

                for (i = 1; i < CONNS && conns[i].used; i++)
                    ;
                if (fd >= 0 && i < CONNS)
                    conn_open(&conns[i], fd);
                else if (fd >= 0)
                    close(fd);
            }
            for (i = 1; i < CONNS; i++) {
                struct conn *c = &conns[i];
                ssize_t n;

                if (!c->used || p[i].fd < 0 ||
                    !(p[i].revents & (POLLIN | POLLHUP)))
                    continue;
                n = recv(c->fd, c->pend + c->npend,
                         sizeof c->pend - c->npend, 0);
                if (n <= 0) {
                    conn_close(c);
                    continue;
                }
                c->npend += (size_t)n;
            }
        }
        for (i = 1; i < CONNS; i++)
            if (conns[i].used && conns[i].npend)
                conn_drain(&conns[i]);
    }
}

int main(int argc, char **argv)
{
    int i;

    int http = -1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--http") && i + 1 < argc)
            http = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace"))
            trace = 1;
        else {
            fprintf(stderr, "usage: %s [--dir DIR] [--http PORT] [--trace]\n",
                    argv[0]);
            return 2;
        }
    }
    if (http >= 0)
        return serve_http(http);
    conn_open(&conns[0], -1);
    cur = &conns[0].t;
    for (;;) {
        struct pollfd in = { 0, POLLIN, 0 };
        char chunk[512];
        ssize_t n;

        /* Wait for input, or until the next count is due. */
        if (poll(&in, 1, count_tick()) == 0)
            continue;
        n = read(0, chunk, sizeof chunk);
        if (n <= 0)
            break;
        if (trace)
            fprintf(stderr, "-> %.*s", (int)n, chunk);
        tinymcp_feed(cur, chunk, (size_t)n);
    }
    conn_close(&conns[0]);
    return 0;
}
