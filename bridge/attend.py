#!/usr/bin/env python3
#
# tinymcp v0.4
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# attend.py - answer a tikuOS board's questions from the Anthropic API, for
# a board left running with no agent attached.  Started by the owner, with
# the owner's key in the environment; the bridge it runs carries no key.
#
# SPDX-License-Identifier: Apache-2.0

"""Answer the questions a board asks on /sys/mcp/ask.

    ANTHROPIC_API_KEY=... python3 bridge/attend.py /dev/cu.usbmodem1101

attend.py is an MCP client of the board through bridge/tinymcp.py.  It
subscribes to tiku:///sys/mcp/ask/question; when the board asks, it sends
the question to the Messages API and writes the reply to
/sys/mcp/ask/answer, cut to one line that fits the board.  The model is
told the board's instructions and the names of the prompts the board holds,
and nothing else.

Each question costs at most --max-tokens (1000) tokens of reply, and at
most --per-hour (20) questions are sent in any hour; past that the answer
is a refusal.  Every exchange is written to /sys/mcp/journal: the question
and answer by the board's own entries, the model and tokens by attend's.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

import anyio

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "tinymcp.py")

QUESTION = "tiku:///sys/mcp/ask/question"
ANSWER_MAX = 180          # bytes of answer; the board keeps 191
JOURNAL_MAX = 70          # bytes of a journal entry's text
API_VERSION = "2023-06-01"
DEFAULT_MODEL = "claude-opus-5-5"


def fit(text, limit=ANSWER_MAX):
    """One line of at most limit bytes of UTF-8, cut between characters."""
    line = " ".join(text.split())
    raw = line.encode("utf-8")
    if len(raw) <= limit:
        return line
    return raw[:limit].decode("utf-8", errors="ignore").rstrip()


class Budget:
    """At most per_hour questions in any sliding hour."""

    def __init__(self, per_hour, clock=time.monotonic):
        self.per_hour = per_hour
        self.clock = clock
        self.sent = []

    def allow(self):
        now = self.clock()
        self.sent = [t for t in self.sent if now - t < 3600.0]
        if len(self.sent) >= self.per_hour:
            return False
        self.sent.append(now)
        return True


def system_prompt(instructions, prompts):
    """What the model is told: the board's instructions and prompt names."""
    lines = ["You answer questions a small computer running tikuOS asks while "
             "no person is attached.  Reply with the answer only, in one line "
             f"of at most {ANSWER_MAX} characters; the board reads it as text.",
             "", "The board describes itself:", instructions or "(nothing)"]
    if prompts:
        lines += ["", "The prompts its owner wrote:"]
        lines += [f"- {name}: {desc}" if desc else f"- {name}"
                  for name, desc in prompts]
    return "\n".join(lines)


def ask_model(question, system, model, max_tokens, key, base_url,
              timeout=60.0):
    """(reply text, input tokens, output tokens) from the Messages API."""
    body = json.dumps({
        "model": model,
        "max_tokens": max_tokens,
        "system": system,
        "messages": [{"role": "user", "content": question}],
    }).encode()
    req = urllib.request.Request(
        base_url.rstrip("/") + "/v1/messages", data=body, method="POST",
        headers={"x-api-key": key, "anthropic-version": API_VERSION,
                 "content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        reply = json.loads(resp.read())
    text = "".join(b.get("text", "") for b in reply.get("content", [])
                   if b.get("type") == "text")
    usage = reply.get("usage", {})
    return text, usage.get("input_tokens", 0), usage.get("output_tokens", 0)


def journal_text(seq, what):
    """attend's journal entry for question seq, cut to fit."""
    return fit(f"#{seq} {what}", JOURNAL_MAX)


class Attendant:
    """One board's questions, answered through an MCP client session."""

    def __init__(self, client, args, key):
        self.c = client
        self.args = args
        self.key = key
        self.budget = Budget(args.per_hour)
        self.system = ""
        self.answered = 0

    async def read(self, path):
        r = await self.c.read_resource("tiku://" + path)
        return r.contents[0].text.strip()

    async def write(self, path, value):
        r = await self.c.call_tool("write", {"path": path, "value": value})
        return not r.is_error

    async def prepare(self, instructions):
        prompts = []
        try:
            for p in (await self.c.list_prompts()).prompts:
                prompts.append((p.name, p.description))
        except Exception:  # noqa: BLE001 - a board without prompts
            pass
        self.system = system_prompt(instructions, prompts)

    async def handle(self):
        """Answer the question waiting, if one is; True when it answered."""
        if await self.read("/sys/mcp/ask/state") != "asked":
            return False
        seq = await self.read("/sys/mcp/ask/seq")
        question = await self.read("/sys/mcp/ask/question")
        a = self.args
        if not self.budget.allow():
            answer = f"attend: past {a.per_hour} questions this hour; ask later"
            note = "refused: the hourly limit"
        else:
            try:
                text, tin, tout = await anyio.to_thread.run_sync(
                    ask_model, question, self.system, a.model, a.max_tokens,
                    self.key, a.base_url)
                answer = fit(text) or "attend: the model gave no text"
                note = f"{a.model} in {tin} out {tout}"
            except (urllib.error.URLError, OSError, ValueError) as e:
                answer = "attend: the API could not be reached"
                note = fit(f"API error: {e}", JOURNAL_MAX - 12)
        ok = await self.write("/sys/mcp/ask/answer", f"#{seq} {answer}")
        await self.write("/sys/mcp/journal", journal_text(seq, note))
        print(f"attend: #{seq} {question!r} -> {answer!r} ({note})"
              f"{'' if ok else ' [not taken: a later question]'}",
              file=sys.stderr, flush=True)
        self.answered += 1
        return True


async def serve(args, key):
    from mcp import Client
    from mcp.client.stdio import StdioServerParameters
    import mcp_types as types
    from mcp.shared.subscriptions import ResourceUpdated

    params = StdioServerParameters(command=sys.executable,
                                   args=[BRIDGE, args.port])
    info = types.Implementation(name="attend", version="1")
    async with Client(params, mode="auto", client_info=info,
                      read_timeout_seconds=30, cache=None) as c:
        at = Attendant(c, args, key)
        await at.prepare(c.instructions)
        async with c.listen(resource_subscriptions=[QUESTION]) as sub:
            await at.handle()           # a question already waiting
            while not (args.once and at.answered):
                try:
                    with anyio.fail_after(args.timeout or None):
                        ev = await sub.__anext__()
                except TimeoutError:
                    break
                if isinstance(ev, ResourceUpdated) and ev.uri == QUESTION:
                    await at.handle()
    return 0 if (at.answered or not args.once) else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", help="the board's serial port")
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--max-tokens", type=int, default=1000,
                    help="the longest reply, in tokens (default 1000)")
    ap.add_argument("--per-hour", type=int, default=20,
                    help="questions sent in any hour (default 20)")
    ap.add_argument("--base-url", default=os.environ.get(
        "ANTHROPIC_BASE_URL", "https://api.anthropic.com"))
    ap.add_argument("--once", action="store_true",
                    help="exit after answering one question")
    ap.add_argument("--timeout", type=float, default=0,
                    help="exit after this many seconds without a question")
    args = ap.parse_args(argv)
    key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not key:
        print("attend: set ANTHROPIC_API_KEY; the key stays in this "
              "environment and never reaches the board", file=sys.stderr)
        return 2
    return anyio.run(serve, args, key)


if __name__ == "__main__":
    sys.exit(main())
