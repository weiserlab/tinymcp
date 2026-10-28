#!/usr/bin/env python3
#
# tinymcp v0.4
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# board_ask.py - the board asks: a BASIC program on a tikuOS board asks a
# question with ASK, and this script, an MCP client on the same port,
# answers it; a second ASK runs out of time; a rule asks when a node
# changes; the journal names who asked and who answered; attend.py, against
# a stub of the Messages API, answers a question the board asks alone.  Every check prints
# one "CHECK basic <key> ok|fail <detail>" line, which TikuBench reads.
#
# SPDX-License-Identifier: Apache-2.0

import http.server
import importlib.util
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "bridge", os.path.join(HERE, "..", "bridge", "tinymcp.py"))
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

CLIENT = "board-ask"
META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
        "io.modelcontextprotocol/clientCapabilities": {},
        "io.modelcontextprotocol/clientInfo": {"name": CLIENT, "version": "1"}}

KEYS = ["basic-ask", "asked-by", "basic-answer", "answered-by", "basic-late",
        "rule-ask", "journal-who", "attend"]
ATTEND = os.path.join(HERE, "..", "bridge", "attend.py")


class StubAPI(http.server.BaseHTTPRequestHandler):
    """The Messages API's shape, answering every question with "amber"."""

    def do_POST(self):
        self.rfile.read(int(self.headers["content-length"]))
        out = json.dumps({"content": [{"type": "text", "text": "amber"}],
                          "usage": {"input_tokens": 9, "output_tokens": 1}}).encode()
        self.send_response(200)
        self.send_header("content-length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *a):
        pass

failures = 0
checks = 0


def expect(key, cond, what):
    global failures, checks
    checks += 1
    print(f"CHECK basic {key} {'ok' if cond else 'fail'} {what}", flush=True)
    if not cond:
        failures += 1


class OurFrames(bridge.Splitter):
    """Frames with tinymcp's marker only.  The bridge's splitter also takes a
    byte from 0x40 to 0x4F after an END as another channel's marker, so a
    program's output that begins with A to O right after a reply would be
    lost; a board running this test carries no other channel."""

    def feed(self, data):
        text = bytearray()
        frames = []
        for b in data:
            if self._armed and b != self.marker and b != bridge.END:
                self._armed = False
                text.append(b)
                continue
            t, f = super().feed(bytes((b,)))
            text += t
            frames += f
        return bytes(text), frames


class Board:
    """The console text and the MCP frames of one port."""

    def __init__(self, port):
        self.link = bridge.SerialLink(port, 115200, raw=False)
        self.link.split = OurFrames()
        self.text = ""
        self.replies = {}
        self.next_id = 1

    def pump(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            text, msgs = self.link.recv()
            if text is None:
                raise SystemExit("the port closed")
            self.text += text.decode("latin-1")
            for m in msgs:
                msg = json.loads(m)
                if "id" in msg:
                    self.replies[msg["id"]] = msg

    def type(self, line, settle=0.3):
        self.link.ser.write(line.encode() + b"\r")
        self.link.ser.flush()
        self.pump(settle)

    def wait_text(self, pattern, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            m = re.search(pattern, self.text)
            if m:
                return m
            self.pump(0.1)
        return None

    def tool(self, name, args, seconds=5):
        rid = self.next_id
        self.next_id += 1
        req = {"jsonrpc": "2.0", "id": rid, "method": "tools/call",
               "params": {"name": name, "arguments": args, "_meta": META}}
        self.link.send(json.dumps(req).encode())
        end = time.monotonic() + seconds
        while rid not in self.replies and time.monotonic() < end:
            self.pump(0.1)
        r = self.replies.pop(rid, {})
        res = r.get("result", {})
        text = "".join(c.get("text", "") for c in res.get("content", []))
        return text, bool(res.get("isError")) or "error" in r

    def read(self, path):
        return self.tool("read", {"path": path})[0].strip()


RULE_ACTION = "ask led1 changed"


def drop_rules(b):
    """Delete the rules this test adds, by their number in the listing."""
    listing, _ = b.tool("shell", {"line": "rules"})
    for n in re.findall(r"#(\d+)\s.*-> " + RULE_ACTION, listing):
        b.tool("shell", {"line": f"rules del {n}"})


def main(port):
    b = Board(port)
    b.type("")
    drop_rules(b)
    b.type("basic", 1.0)
    for line in ("NEW",
                 '10 ASK "Which colour?", C$, 30',
                 '20 PRINT "GOT ["; C$; "]"',
                 '30 ASK "Again?", D$, 2',
                 '40 PRINT "LATE ["; D$; "]"',
                 "50 END",
                 "RUN"):
        b.type(line, 0.2)

    # The program asks; the question waits for its answer.
    seq = ""
    end = time.monotonic() + 10
    while time.monotonic() < end:
        if b.read("/sys/mcp/ask/state") == "asked":
            seq = b.read("/sys/mcp/ask/seq")
            break
        b.pump(0.3)
    q = b.read("/sys/mcp/ask/question")
    waiting = re.search(r'GOT \[[^\]"]*\]', b.text) is None
    expect("basic-ask", seq != "" and q == "Which colour?" and waiting,
           f"#{seq} {q!r}, program waiting: {waiting}")
    who = b.read("/sys/mcp/ask/asked_by")
    expect("asked-by", who == "board", f"asked_by {who!r}")

    text, err = b.tool("write", {"path": "/sys/mcp/ask/answer",
                                 "value": f"#{seq} green"})
    m = b.wait_text(r'GOT \[([^\]"]*)\]', 10)
    expect("basic-answer", not err and m is not None and m.group(1) == "green",
           f"write {text!r}; program printed {m.group(0) if m else None!r}")

    # The journal names who asked and who answered.
    journal = b.read("/sys/mcp/journal")
    lines = journal.splitlines()
    answered = [x for x in lines if f"answered #{seq}:" in x]
    expect("answered-by", any(x.split(" ", 2)[1:] == [CLIENT, f"answered #{seq}: green"]
                              for x in answered), f"{answered}")
    expect("journal-who", any(x.endswith(f" board asked #{seq}: Which colour?")
                          for x in lines), " | ".join(lines[-6:]))
    m = b.wait_text(r'LATE \[([^\]"]*)\]', 10)
    expect("basic-late", m is not None and m.group(1) == "",
           f"the unanswered ASK printed {m.group(0) if m else None!r}")
    b.type("BYE", 1.0)

    # A rule asks when a node changes.
    text, _ = b.tool("shell", {"line": "on changed /dev/led1 " + RULE_ACTION})
    before = b.read("/sys/mcp/ask/seq")
    b.tool("write", {"path": "/dev/led1", "value": "1"})
    b.tool("write", {"path": "/dev/led1", "value": "0"})
    end = time.monotonic() + 5
    while b.read("/sys/mcp/ask/seq") == before and time.monotonic() < end:
        b.pump(0.3)
    q = b.read("/sys/mcp/ask/question")
    expect("rule-ask", q == "led1 changed" and b.read("/sys/mcp/ask/seq") != before,
           f"rule {text.strip()!r}; question {q!r}")
    drop_rules(b)
    left, _ = b.tool("shell", {"line": "rules"})
    if RULE_ACTION in left:
        expect("rule-ask", False, f"the test's rule stayed: {left!r}")

    # The board asks with nobody attached; attend.py answers from the API.
    b.tool("shell", {"line": "once 4 ask What should led2 show?"})
    b.link.close()
    srv = http.server.HTTPServer(("127.0.0.1", 0), StubAPI)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    env = dict(os.environ, ANTHROPIC_API_KEY="sk-test")
    run = subprocess.run([sys.executable, ATTEND, port, "--once", "--timeout", "30",
                          "--base-url", f"http://127.0.0.1:{srv.server_port}"],
                         env=env, capture_output=True, text=True, timeout=90)
    srv.shutdown()
    b = Board(port)
    q = b.read("/sys/mcp/ask/question")
    answer = b.read("/sys/mcp/ask/answer")
    who = b.read("/sys/mcp/ask/answered_by")
    journal = b.read("/sys/mcp/journal")
    expect("attend", run.returncode == 0 and q == "What should led2 show?" and
           answer == "amber" and who == "attend" and
           "claude-opus-5-5 in 9 out 1" in journal,
           f"rc {run.returncode}; {q!r} -> {answer!r} by {who!r}; "
           f"{(run.stderr.strip().splitlines() or [''])[-1]!r}")
    b.link.close()
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: board_ask.py <serial port>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
