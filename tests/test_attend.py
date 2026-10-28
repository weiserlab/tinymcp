#!/usr/bin/env python3
#
# tinymcp v0.5
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# test_attend.py - attend.py against a stub of the Messages API and a fake
# board: the request it sends, the answer cut to fit, the hourly limit, the
# refusal past it, an API failure, and the journal entries it writes.
#
# SPDX-License-Identifier: Apache-2.0

import http.server
import json
import os
import sys
import threading
from types import SimpleNamespace

import anyio

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "bridge"))
import attend  # noqa: E402

failures = 0
checks = 0


def expect(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL: {what}")


class Stub(http.server.BaseHTTPRequestHandler):
    """The Messages API: records each request, answers with Stub.reply."""
    seen = []
    reply = {}
    status = 200

    def do_POST(self):
        body = self.rfile.read(int(self.headers["content-length"]))
        Stub.seen.append((self.path, {k.lower(): v for k, v in self.headers.items()},
                          json.loads(body)))
        out = json.dumps(Stub.reply).encode()
        self.send_response(Stub.status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass


class Board:
    """The /sys/mcp/ask nodes and the journal, as attend's client sees them."""

    def __init__(self):
        self.nodes = {"/sys/mcp/ask/state": "asked", "/sys/mcp/ask/seq": "7",
                      "/sys/mcp/ask/question": "LED colour at night?"}
        self.writes = []

    async def read_resource(self, uri):
        text = self.nodes[uri[len("tiku://"):]] + "\n"
        return SimpleNamespace(contents=[SimpleNamespace(text=text)])

    async def call_tool(self, name, args):
        self.writes.append((args["path"], args["value"]))
        if args["path"] == "/sys/mcp/ask/answer":
            self.nodes["/sys/mcp/ask/state"] = "answered"
        return SimpleNamespace(is_error=False)

    async def list_prompts(self):
        return SimpleNamespace(prompts=[
            SimpleNamespace(name="night", description="What to do at night")])


def pure():
    expect(attend.fit("a\nb  c") == "a b c", "fit joins lines")
    s = attend.fit("é" * 200)
    expect(len(s.encode()) <= attend.ANSWER_MAX and s == "é" * 90,
           f"fit cuts between characters: {len(s.encode())} bytes")
    t = [0.0]
    b = attend.Budget(2, clock=lambda: t[0])
    expect(b.allow() and b.allow() and not b.allow(), "two an hour, not three")
    t[0] = 3601.0
    expect(b.allow(), "the hour slides")
    j = attend.journal_text(12, "claude-opus-5-5 in 1234 out 567 " + "x" * 80)
    expect(len(j.encode()) <= attend.JOURNAL_MAX and j.startswith("#12 claude"),
           f"journal entry fits: {j!r}")
    sp = attend.system_prompt("tikuOS on nRF54L15.", [("night", "At night"),
                                                     ("bare", None)])
    expect("tikuOS on nRF54L15." in sp and "- night: At night" in sp and
           "- bare" in sp and str(attend.ANSWER_MAX) in sp,
           "the system prompt holds the instructions and prompt names")


async def session(base):
    args = SimpleNamespace(model="claude-opus-5-5", max_tokens=1000,
                           per_hour=1, base_url=base)
    board = Board()
    at = attend.Attendant(board, args, "sk-test")
    await at.prepare("tikuOS on nRF54L15.")

    Stub.reply = {"content": [{"type": "text", "text": "Dim red,\nso eyes adapt."}],
                  "usage": {"input_tokens": 41, "output_tokens": 6}}
    expect(await at.handle(), "a waiting question is answered")
    path, headers, body = Stub.seen[-1]
    expect(path == "/v1/messages", f"the endpoint {path}")
    expect(headers.get("x-api-key") == "sk-test" and
           headers.get("anthropic-version") == attend.API_VERSION,
           "the key and the API version go in the headers")
    expect(body["model"] == "claude-opus-5-5" and body["max_tokens"] == 1000 and
           body["messages"] == [{"role": "user", "content": "LED colour at night?"}],
           f"the request body {body}")
    expect("What to do at night" in body["system"], "the prompts are in the system prompt")
    expect(("/sys/mcp/ask/answer", "#7 Dim red, so eyes adapt.") in board.writes,
           f"the answer names its question: {board.writes}")
    expect(("/sys/mcp/journal", "#7 claude-opus-5-5 in 41 out 6") in board.writes,
           "the journal gets the model and tokens")
    expect(not await at.handle(), "an answered question is left alone")

    # Past the hourly limit the answer is a refusal and the API is not called.
    n = len(Stub.seen)
    board.nodes.update({"/sys/mcp/ask/state": "asked", "/sys/mcp/ask/seq": "8"})
    await at.handle()
    expect(len(Stub.seen) == n and board.writes[-2][1].startswith("#8 attend: past 1"),
           f"refusal past the limit: {board.writes[-2:]}")
    expect(board.writes[-1] == ("/sys/mcp/journal", "#8 refused: the hourly limit"),
           "the refusal is journaled")

    # An API that fails gives a plain answer, not silence.
    at.budget = attend.Budget(5)
    Stub.status = 500
    board.nodes.update({"/sys/mcp/ask/state": "asked", "/sys/mcp/ask/seq": "9"})
    await at.handle()
    Stub.status = 200
    expect(board.writes[-2] == ("/sys/mcp/ask/answer",
                                "#9 attend: the API could not be reached"),
           f"an API failure answers: {board.writes[-2]}")
    expect(board.writes[-1][1].startswith("#9 API error"), "and is journaled")


def main():
    srv = http.server.HTTPServer(("127.0.0.1", 0), Stub)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    pure()
    anyio.run(session, f"http://127.0.0.1:{srv.server_port}")
    srv.shutdown()
    env = dict(os.environ)
    env.pop("ANTHROPIC_API_KEY", None)
    os.environ.pop("ANTHROPIC_API_KEY", None)
    expect(attend.main(["/dev/null"]) == 2, "no key, no start")
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
