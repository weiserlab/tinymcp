#!/usr/bin/env python3
#
# tinymcp v0.1
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# test_bridge.py - the bridge against a fake device on a pty and on a TCP
# socket: framing both ways, text kept off stdout, escapes, --raw, and the
# cancellations it sends for open listens when its stdin closes.
#
# SPDX-License-Identifier: Apache-2.0

import json
import os
import pty
import select
import socket
import subprocess
import sys
import threading
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "bridge", "tinymcp.py")
sys.path.insert(0, os.path.join(HERE, "..", "bridge"))
import tinymcp as bridge  # noqa: E402

failures = 0
checks = 0


def expect(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL: {what}")


def read_until(fd, needle, timeout=5.0):
    """Bytes from fd until needle appears or the time is up."""
    buf = bytearray()
    end = time.time() + timeout
    while needle not in buf and time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                buf += os.read(fd, 4096)
            except OSError:
                break
    return bytes(buf)


def read_line(stream, timeout=5.0):
    out = []

    def work():
        out.append(stream.readline())

    t = threading.Thread(target=work, daemon=True)
    t.start()
    t.join(timeout)
    return out[0] if out else b""


def start(args, port):
    p = subprocess.Popen([sys.executable, BRIDGE] + args + ([port] if port else []),
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    opened = read_line(p.stderr)
    expect(b"opened" in opened, f"bridge opened: {opened!r}")
    return p


def open_pty():
    """A pty whose slave only the bridge will hold: its name, and the master."""
    master, slave = pty.openpty()
    tty.setraw(master)
    name = os.ttyname(slave)
    os.close(slave)
    return master, name


def serial_framed():
    master, name = open_pty()
    p = start([], name)
    time.sleep(0.2)
    req = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
    p.stdin.write(req + b"\n")
    p.stdin.flush()
    got = read_until(master, b"\xc0", timeout=5)
    got += read_until(master, b"\xc0", timeout=2) if got.count(b"\xc0") < 2 else b""
    expect(got == bridge.frame(req), f"request framed on the wire: {got!r}")

    reply = b'{"jsonrpc":"2.0","id":1,"result":{}}'
    os.write(master, b"tikuOS:/> banner text\r\n" + bridge.frame(reply) + b"more text")
    line = read_line(p.stdout)
    expect(line == reply + b"\n", f"reply on stdout: {line!r}")
    err = read_until(p.stderr.fileno(), b"more text", timeout=3)
    expect(b"banner text" in err and b"more text" in err and reply not in err,
           f"text on stderr only: {err!r}")

    # Escaped bytes inside a frame come out whole; other channels are dropped.
    odd = b'{"id":2,"x":"\xc0\xdb\x00"}'
    os.write(master, bridge.frame(b"not ours", marker=0xF1) + bridge.frame(odd))
    line = read_line(p.stdout)
    expect(line == odd + b"\n", f"escapes decoded: {line!r}")

    # A listen, then EOF: the bridge cancels it before leaving.
    listen = b'{"jsonrpc":"2.0","id":"listen-7","method":"subscriptions/listen","params":{"notifications":{}}}'
    p.stdin.write(listen + b"\n")
    p.stdin.flush()
    read_until(master, b"\xc0", timeout=3)
    read_until(master, b"\xc0", timeout=1)
    p.stdin.close()
    got = read_until(master, b"\xc0", timeout=3)
    got += read_until(master, b"\xc0", timeout=1)
    text, frames = bridge.Splitter().feed(got)
    expect(len(frames) == 1 and json.loads(frames[0]) == {
        "jsonrpc": "2.0", "method": "notifications/cancelled",
        "params": {"requestId": "listen-7"}}, f"cancelled on exit: {frames}")
    expect(p.wait(timeout=5) == 0, "bridge exits after stdin closes")
    os.close(master)


def serial_raw():
    master, name = open_pty()
    p = start(["--raw"], name)
    time.sleep(0.2)
    req = b'{"jsonrpc":"2.0","id":3,"method":"ping"}'
    p.stdin.write(req + b"\n")
    p.stdin.flush()
    got = read_until(master, b"\n", timeout=3)
    expect(got == req + b"\n", f"raw request: {got!r}")
    os.write(master, b'{"jsonrpc":"2.0","id":3,"res')
    os.write(master, b'ult":{}}\r\n{"jsonrpc":"2.0","id":4,"result":{}}\n')
    a = read_line(p.stdout)
    b = read_line(p.stdout)
    expect(a == b'{"jsonrpc":"2.0","id":3,"result":{}}\n' and
           b == b'{"jsonrpc":"2.0","id":4,"result":{}}\n',
           f"raw replies reassembled across reads: {a!r} {b!r}")
    p.stdin.close()
    expect(p.wait(timeout=5) == 0, "raw bridge exits")
    os.close(master)


def tcp():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]
    p = start(["--tcp", f"127.0.0.1:{port}"], None)
    conn, _ = srv.accept()
    conn.settimeout(5)
    req = b'{"jsonrpc":"2.0","id":5,"method":"ping"}'
    p.stdin.write(req + b"\n")
    p.stdin.flush()
    got = conn.recv(4096)
    expect(got == req + b"\n", f"tcp request: {got!r}")
    conn.sendall(b'{"jsonrpc":"2.0","id":5,"result":{}}\n')
    line = read_line(p.stdout)
    expect(line == b'{"jsonrpc":"2.0","id":5,"result":{}}\n', f"tcp reply: {line!r}")
    conn.close()
    srv.close()
    p.stdin.close()
    expect(p.wait(timeout=5) == 0, "tcp bridge exits")


def held_port():
    master, name = open_pty()
    with open(name, "rb") as held:
        holder = subprocess.Popen(["sleep", "30"], stdin=held)
    time.sleep(0.2)
    p = subprocess.Popen([sys.executable, BRIDGE, name], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    _, err = p.communicate(timeout=15)
    expect(p.returncode != 0 and b"held by pid" in err, f"held port refused: {err!r}")
    holder.kill()
    holder.wait()
    os.close(master)


def unit():
    s = bridge.Splitter()
    text, frames = s.feed(b"ab\xc0\xf3hi\xdb\xdc\xdb\xdd\xdb\xde\xc0cd\xc0zz\xc0\xf3")
    expect(text == b"abcdzz" and frames == [b"hi\xc0\xdb\x00"], f"splitter: {text!r} {frames!r}")
    text, frames = s.feed(b"tail\xc0")
    expect(text == b"" and frames == [b"tail"], f"splitter across feeds: {frames!r}")
    expect(bridge.escape(b"\xc0\xdb\x00x") == b"\xdb\xdc\xdb\xdd\xdb\xdex", "escape")
    expect(bridge.listen_ids(b'{"id":9,"method":"subscriptions/listen"}') == 9, "listen id")
    expect(bridge.listen_ids(b'{"id":9,"method":"ping"}') is None, "not a listen")
    expect(bridge.listen_ids(b"junk") is None, "junk line")


if __name__ == "__main__":
    unit()
    serial_framed()
    serial_raw()
    tcp()
    if sys.platform != "win32":
        held_port()
    print(f"{checks} checks, {failures} failures")
    sys.exit(1 if failures else 0)
