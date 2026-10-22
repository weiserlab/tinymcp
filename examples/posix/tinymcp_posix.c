/*
 * tinymcp v0.1
 * The Model Context Protocol server as a library any firmware can carry.
 * http://tinymcp.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tinymcp_posix.c - an MCP server over stdin and stdout that serves the
 * files of one directory as resources and runs lines through the shell: the
 * conformance target, and the shape a binding takes on any host.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tinymcp.h"

#define IN_CAP  16384
#define OUT_CAP 32768
#define NAME_MAX_LEN 128

static char in_buf[IN_CAP], out_buf[OUT_CAP];
static tinymcp_t server;
static const char *dir = ".";
static int trace;
static char watched[TINYMCP_SUBS_MAX][NAME_MAX_LEN];

static const char *const uri_prefix = "file:///";

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

/* The directory's regular files, sorted, as resources from cursor on. */
static int list(void *ctx, unsigned cursor, tinymcp_list_t *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char *names[256];
    unsigned n = 0, i;
    int next = 0;

    (void)ctx;
    if (!d)
        return TINYMCP_EIO;
    while ((e = readdir(d)) != NULL && n < 256) {
        char path[512];
        struct stat st;

        if (e->d_name[0] == '.' || path_of(e->d_name, path, sizeof path))
            continue;
        if (stat(path, &st) || !S_ISREG(st.st_mode))
            continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof names[0], cmp_names);
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
static int read_file(void *ctx, const char *uri, unsigned line, unsigned bytes,
                     tinymcp_sink_t *text)
{
    const char *name = uri_name(uri);
    char path[512];
    FILE *f;
    int c;
    unsigned skipped = 0, written = 0;

    (void)ctx;
    if (!name || path_of(name, path, sizeof path))
        return TINYMCP_ENOENT;
    f = fopen(path, "r");
    if (!f)
        return errno == ENOENT ? TINYMCP_ENOENT : TINYMCP_EIO;
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
      "\"description\":\"the command line\"}},\"required\":[\"line\"]}" },
    { "write", "Write text to a file in the served directory, creating it.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
      "\"text\":{\"type\":\"string\"}},\"required\":[\"name\",\"text\"]}" },
    { "echo", "Return the text given.",
      "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
      "\"required\":[\"text\"]}" },
};

static const tinymcp_template_t templates[] = {
    { "file:///{name}", "a file", "a file of the served directory" },
    { "file:///{name}{?line,bytes}", "a page of a file",
      "from line, at most bytes" },
};

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
        int existed;
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
        for (i = 0; i < TINYMCP_SUBS_MAX; i++)
            if (!strcmp(watched[i], fname))
                tinymcp_updated(&server, i);
        if (!existed)
            tinymcp_listchanged(&server);
        return TINYMCP_OK;
    }
    return TINYMCP_EINVAL;
}

static int watch(void *ctx, const char *uri, unsigned slot)
{
    const char *name = uri_name(uri);
    char path[512];

    (void)ctx;
    if (!name || path_of(name, path, sizeof path) || access(path, F_OK))
        return -1;
    strncpy(watched[slot], name, NAME_MAX_LEN - 1);
    watched[slot][NAME_MAX_LEN - 1] = 0;
    return 0;
}

static void unwatch(void *ctx, unsigned slot)
{
    (void)ctx;
    watched[slot][0] = 0;
}

static int send_line(void *ctx, const char *msg, size_t len)
{
    (void)ctx;
    if (trace)
        fprintf(stderr, "<- %s\n", msg);
    if (fwrite(msg, 1, len, stdout) != len || fputc('\n', stdout) == EOF)
        return -1;
    fflush(stdout);
    return 0;
}

static const tinymcp_host_t host = {
    "tinymcp-posix", TINYMCP_VERSION,
    "The files of one directory are resources; the shell tool runs a line "
    "where the server runs.",
    tools, sizeof tools / sizeof tools[0],
    templates, sizeof templates / sizeof templates[0],
    list, read_file, call, watch, unwatch, send_line
};

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "--trace"))
            trace = 1;
        else {
            fprintf(stderr, "usage: %s [--dir DIR] [--trace]\n", argv[0]);
            return 2;
        }
    }
    tinymcp_init(&server, &host, NULL, in_buf, sizeof in_buf, out_buf,
                 sizeof out_buf, TINYMCP_SUBS_MAX);
    for (;;) {
        char chunk[512];
        ssize_t n = read(0, chunk, sizeof chunk);

        if (n <= 0)
            break;
        if (trace)
            fprintf(stderr, "-> %.*s", (int)n, chunk);
        tinymcp_feed(&server, chunk, (size_t)n);
    }
    tinymcp_closed(&server);
    return 0;
}
