#!/bin/sh
#
# tinymcp v0.1
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
mutate cancel-announces tinymcp.c 's/listen_end(r->t, id, 0);/listen_end(r->t, id, 1);/'
mutate no-cut-marker tinymcp.c 's/tinymcp_jw_raw(w, "\\\\n\[cut\]");/tinymcp_jw_raw(w, "");/'
mutate caps-unchecked tinymcp.c 's/if (tinymcp_json_get(&meta, META_CAPS, &v) != 1 ||/if (0 \&\& tinymcp_json_get(\&meta, META_CAPS, \&v) != 1 ||/'
mutate version-unchecked tinymcp.c 's/if (!version_supported(&pv)) {/if (0) {/'
mutate notify-inside-call tinymcp.c 's/if (t->busy) {/if (0) {/'
mutate in-off-by-one tinymcp.c 's/if (t->in_len + 1 >= t->in_cap) {/if (t->in_len >= t->in_cap) {/'
mutate legacy-era-sticky tinymcp.c 's/r->t->era = TINYMCP_ERA_LEGACY;/r->t->era = TINYMCP_ERA_MODERN;/'
mutate quote-escape tinymcp_json.c "s/e\\[1\\] = '\"';/e[1] = 'Q';/"
mutate surrogate-pair tinymcp_json.c 's/cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);/cp = 0xFFFD;/'
mutate separator tinymcp_json.c "s/if (c == '{' || c == '\\[' || c == ':')/if (0)/"
mutate writer-off-by-one tinymcp_json.c 's/if (w->len + n >= w->cap) {/if (w->len + n > w->cap) {/'
mutate sink-limit tinymcp_json.c 's/if (s->w->len + k > s->limit || s->w->len + k >= s->w->cap) {/if (s->w->len + k >= s->w->cap) {/'

echo "$total mutants, $survived survived"
[ "$survived" -eq 0 ]
