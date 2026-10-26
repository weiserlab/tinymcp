#!/bin/sh
#
# tinymcp v0.3
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# install_hooks.sh - point this repository at hygiene/githooks.
#
# core.hooksPath is per-repository local config, so a clone starts without
# it.  Run once after cloning.
#
# SPDX-License-Identifier: Apache-2.0

set -e

hygiene=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
root=$(CDPATH= cd -- "$hygiene/.." && pwd -P)
hooks="$hygiene/githooks"

[ -d "$hooks" ] || { echo "no githooks in $hygiene" >&2; exit 1; }
[ -e "$root/.git" ] || { echo "$root is not a git checkout" >&2; exit 1; }

git -C "$root" config core.hooksPath "$hooks"
printf '  %-28s hooksPath -> %s\n' \
    "$(basename "$root")" "$(git -C "$root" config core.hooksPath)"

echo
echo "commit messages are now checked against hygiene/commit-style.md"
