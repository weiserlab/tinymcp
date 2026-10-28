#!/bin/sh
#
# tinymcp v0.7
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# mutants.sh - build the test suite against a broken library, one break at a
# time, and refuse any break the suite does not catch.
#
# SPDX-License-Identifier: Apache-2.0

set -u
cd "$(dirname "$0")/.." || exit 1
CC=${CC:-cc}
CFLAGS="-std=c99 -Wall -O1 -g -fsanitize=address,undefined -I. -Ibuild/mutant"
mkdir -p build/mutant
survived=0
total=0

# mutate <name> <file> <sed expression>
mutate() {
    name=$1; file=$2; expr=$3
    total=$((total + 1))
    cp tinymcp.c tinymcp_json.c tinymcp.h tinymcp_json.h build/mutant/
    sed -e "$expr" "$file" > "build/mutant/$file.tmp" && mv "build/mutant/$file.tmp" "build/mutant/$file"
    if cmp -s "$file" "build/mutant/$file"; then
        echo "mutant $name: the pattern no longer matches; update the script"
        survived=$((survived + 1))
        return
    fi
    if ! $CC $CFLAGS -o build/mutant/test tests/test_tinymcp.c \
            build/mutant/tinymcp.c build/mutant/tinymcp_json.c 2>build/mutant/cc.log; then
        echo "mutant $name: caught at compile time"
        return
    fi
    if ./build/mutant/test >build/mutant/run.log 2>&1; then
        echo "mutant $name: SURVIVED"
        survived=$((survived + 1))
    else
        echo "mutant $name: caught"
    fi
}

mutate unknown-method-code tinymcp.c 's/error_reply(r, -32601, "unknown method")/error_reply(r, -32600, "unknown method")/'
mutate result-type tinymcp.c 's/"resultType\\":\\"complete\\",\\"_meta/"resultType\\":\\"partial\\",\\"_meta/'
mutate legacy-not-found tinymcp.c 's/return r->modern ? -32602 : -32002;/return -32602;/'
mutate no-tail-reserve tinymcp.c 's/#define TAIL_RESERVE 80/#define TAIL_RESERVE 0/'
mutate no-eviction tinymcp.c 's/while (needed \&\& slots_free(t) < needed)/while (0)/'
mutate cancel-announces tinymcp.c 's/    listen_end(t, id, 0);/    listen_end(t, id, 1);/'
mutate no-cut-marker tinymcp.c 's/tinymcp_jw_raw(w, "\\\\n\[cut\]");/tinymcp_jw_raw(w, "");/'
mutate caps-unchecked tinymcp.c 's/if (tinymcp_json_get(&meta, META_CAPS, &v) != 1 ||/if (0 \&\& tinymcp_json_get(\&meta, META_CAPS, \&v) != 1 ||/'
mutate version-unchecked tinymcp.c 's/if (!version_supported(&pv)) {/if (0) {/'
mutate notify-inside-call tinymcp.c 's/if (t->busy) {/if (0) {/'
mutate in-off-by-one tinymcp.c 's/if (t->in_len + 1 >= t->in_cap) {/if (t->in_len >= t->in_cap) {/'
mutate legacy-era-sticky tinymcp.c 's/r->t->era = TINYMCP_ERA_LEGACY;/r->t->era = TINYMCP_ERA_MODERN;/'
mutate quote-escape tinymcp_json.c "s/e\\[1\\] = '\"';/e[1] = 'Q';/"
mutate surrogate-pair tinymcp_json.c 's/cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);/cp = 0xFFFD;/'
mutate separator tinymcp_json.c "s/if (c == '{' || c == '\\[' || c == ':')/if (0)/"
mutate writer-off-by-one tinymcp_json.c 's/while (w->len + n >= w->cap) {/while (w->len + n > w->cap) {/'
mutate sink-limit tinymcp_json.c 's/    return s->w->len + k <= s->limit \&\& s->w->len + k < s->w->cap;/    return s->w->len + k < s->w->cap;/'

mutate no-title tinymcp.c 's/    if (h->title) {/    if (0) {/'
mutate no-library-meta tinymcp.c 's/        if (library)$/        if (0)/'
mutate no-prompts-cap tinymcp.c 's/    if (h->prompt_list) {/    if (0) {/'
mutate no-completions-cap tinymcp.c 's/    if (h->complete) {/    if (0) {/'
mutate required-always tinymcp.c 's/args\[i\].required ? "true" : "false"/"true"/'
mutate prompt-einval tinymcp.c 's/    if (rc == TINYMCP_EINVAL) {/    if (0) {/'
mutate name-rescanned tinymcp.c 's/    \*name = decoded(&n);/    *name = param_str(r, "name");/'
mutate complete-uncapped tinymcp.c 's/    if (c->count >= TINYMCP_COMPLETE_MAX)/    if (0)/'
mutate has-more tinymcp.c 's/c.offered > c.count ? "true"/c.offered >= c.count ? "true"/'
mutate no-context tinymcp.c 's/tinymcp_json_get(&ctx, "arguments", &ctx_args) != 1)/1)/'
mutate prompts-filter tinymcp.c 's/        want_lc |= TINYMCP_LC_PROMPTS;/        want_lc |= TINYMCP_LC_RESOURCES;/'
mutate prompts-method tinymcp.c 's/? "notifications\/prompts\/list_changed"/? "notifications\/resources\/list_changed"/'
mutate prompts-pending tinymcp.c 's/        t->pending_lc |= TINYMCP_LC_PROMPTS;/        t->pending_lc |= 0;/'

mutate render-unchecked tinymcp.c 's/        if (k \&\& !arg_strn(args, text + i + 2, k))/        if (0)/'
mutate placeholder-long tinymcp.c 's/i - 2 < TINYMCP_PNAME_MAX - 1 \&\&/i - 2 < TINYMCP_PNAME_MAX \&\&/'
mutate placeholder-dup tinymcp.c 's/        if (j == count \&\& count < max) {/        if (count < max) {/'

mutate client-per-request tinymcp.c 's/            t->client\[0\] = 0;$/            ;/'
mutate client-legacy tinymcp.c 's/    if (param(r, "clientInfo", \&v))/    if (0)/'
mutate client-cut tinymcp.c 's/        if (tinymcp_json_decode(t->client, n) >= 0)/        if (1)/'
mutate client-closed tinymcp.c '/    t->legacy_version = NULL;/{n;s/    t->client\[0\] = 0;/    ;/;}'

mutate input-required-type tinymcp.c 's/{\\"resultType\\":\\"input_required\\",/{\\"resultType\\":\\"complete\\",/'
mutate elicits-always tinymcp.c 's/            t->elicits = tinymcp_json_get(\&v, "elicitation", \&el) == 1 \&\&/            t->elicits = 1 || (tinymcp_json_get(\&v, "elicitation", \&el) == 1) ||/'
mutate input-not-reset tinymcp.c '/^out:$/,/flush_pending/{s/    t->in_responses.type = TINYMCP_J_NONE;/    ;/;}'
mutate elicit-not-reset tinymcp.c '/^out:$/,/flush_pending/{s/    t->el_key = t->el_message = t->el_schema = t->el_state = NULL;/    ;/;}'
mutate state-no-decode tinymcp.c 's/    return tinymcp_json_decode(buf, t->in_state.len);/    buf[t->in_state.len] = 0; return (int)t->in_state.len;/'
mutate decline-is-accept tinymcp.c 's/    if (tinymcp_json_is(\&action, "decline"))/    if (tinymcp_json_is(\&action, "accept"))/'
mutate state-fits tinymcp.c 's/    if (t->in_state.type != TINYMCP_J_STRING || t->in_state.len + 1 > cap)/    if (t->in_state.type != TINYMCP_J_STRING || t->in_state.len > cap)/'
mutate need-input-tool tinymcp.c '/rc = h->tool_call/,/host_failed/{s/    if (rc == TINYMCP_NEED_INPUT) {/    if (0) {/;}'

mutate later-cancel-host tinymcp.c 's/    if (cancelled \&\& t->host->cancel)/    if (0)/'
mutate later-cancel-match tinymcp.c 's/        if (t->later\[i\].used \&\& !strcmp(t->later\[i\].id, id)) {/        if (0) {/'
mutate later-closed-silent tinymcp.c '/Nobody is left/,/later_drop/s/later_drop(t, i, 1)/later_drop(t, i, 0)/'
mutate later-progress-inside tinymcp.c 's/    if (!l || !l->token\[0\] || t->busy)/    if (!l || !l->token[0])/'
mutate later-begin-inside tinymcp.c '/^int tinymcp_result_begin/,/^}/s/    if (!l || t->busy)$/    if (!l)/'
mutate later-total-always tinymcp.c 's/    if (total) {/    if (1) {/'
mutate later-defer-outside tinymcp.c '/^unsigned tinymcp_defer/,/^}/s/    if (!t->in_call)$/    if (0)/'
mutate later-defer-twice tinymcp.c '/^unsigned tinymcp_defer/,/^}/s/    if (t->deferred)$/    if (0)/'
mutate later-unkept tinymcp.c 's/        if (!r->t->deferred)$/        if (0)/'
mutate later-answered-now tinymcp.c '/Kept and answered now/,/memset/s/    if (r->t->deferred)$/    if (0)/'
mutate later-era tinymcp.c 's/        l->modern = t->cur_modern;/        l->modern = 1;/'
mutate later-no-token tinymcp.c 's/        t->cur_token = v;/        (void)v;/'
mutate later-long-id tinymcp.c 's/        if (id_text(\&t->cur_id, l->id, sizeof l->id) != 0)/        if (0)/'
mutate later-no-flush tinymcp.c '/^void tinymcp_result_end/,/^}/s/    flush_pending(t);/    ;/'
mutate later-busy-stuck tinymcp.c '/^void tinymcp_result_end/,/^}/s/    t->busy = 0;/    ;/'
mutate later-not-dropped tinymcp.c 's/    later_drop(t, call - 1, 0);/    ;/'
mutate later-full tinymcp.c '/^void tinymcp_result_end/,/^}/s/    if (w->full \&\& !w->sent) {/    if (0) {/'
mutate later-end-unopened tinymcp.c 's/    if (!l || t->later_open != call)/    if (!l)/'
mutate later-state-left tinymcp.c '/^out:$/,/busy = 0/s/    t->deferred = 0;/    ;/'

mutate stream-off tinymcp.c 's/    if (t->host->send_part)$/    if (0)/'
mutate emit-whole tinymcp.c '/^static void emit/,/^}/s/    if (w->sent) {$/    if (0) {/'
mutate stream-limited tinymcp_json.c '/^static int sink_room/,/^}/s/    if (s->w->flush)$/    if (0)/'
mutate b64-pad tinymcp_json.c 's/    g\[2\] = s->held > 1 ?/    g[2] = s->held > 0 ?/'
mutate b64-alphabet tinymcp_json.c 's/"0123456789+\/";/"0123456789-_";/'
mutate binary-after-text tinymcp_json.c 's/    if (s->opened || s->kind == TINYMCP_SINK_OPEN)/    if (s->kind == TINYMCP_SINK_OPEN)/'
mutate tool-any-type tinymcp_json.c '/type = "audio"/,/return -1/s/            return -1;/            type = "image";/'
mutate end-no-open tinymcp_json.c '/^void tinymcp_sink_end/,/^}/s/    sink_open(s);/    ;/'
mutate end-no-tail tinymcp_json.c '/^void tinymcp_sink_end/,/^}/s/        b64_group(s);/        ;/'
mutate blob-cut tinymcp.c 's/    if (sink.cut \&\& sink.b64) {/    if (0) {/'
mutate content-bin-cut tinymcp.c 's/    if (sink->cut \&\& sink->b64) {/    if (0) {/'
mutate structured-dropped tinymcp.c 's/    if (t->structured) {/    if (0) {/'
mutate structured-sticky tinymcp.c '/^out:$/,/busy = 0/s/    t->structured = NULL;/    ;/'
mutate output-schema tinymcp.c 's/        if (tool->output_schema) {/        if (0) {/'
mutate late-read-error tinymcp.c '/it ends, short/,/errors++/s/        r->t->errors++;/        ;/'
mutate late-tool-error tinymcp.c '/a failure is isError/,/rc = TINYMCP_TOOL_ERROR;/s/        rc = TINYMCP_TOOL_ERROR;/        ;/'

mutate meta-fits tinymcp.c 's/        v.type != TINYMCP_J_STRING || v.len + 1 > cap)/        v.type != TINYMCP_J_STRING || v.len > cap)/'
mutate meta-any-type tinymcp.c '/^int tinymcp_meta/,/^}/s/        v.type != TINYMCP_J_STRING || /        /'
mutate meta-left tinymcp.c '/^out:$/,/busy = 0/s/    t->cur_meta.type = TINYMCP_J_NONE;/    ;/'
mutate meta-not-kept tinymcp.c 's/        t->cur_meta = meta;/        ;/'

echo "$total mutants, $survived survived"
[ "$survived" -eq 0 ]
