#!/usr/bin/env python3
#
# tinymcp v0.8
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# board_tcp.py - the official mcp SDK client against a tikuOS board's TCP
# port, through the bridge's --tcp and the slip_tcp relay over the board's
# console line: read-only without the key, the owner's authority with it.
#
# SPDX-License-Identifier: Apache-2.0

import json
import os
import re
import sys
import threading
import time

import anyio
import mcp_types as types
import serial
from mcp import Client
from mcp.client.stdio import StdioServerParameters
from mcp.shared.subscriptions import ResourceUpdated

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slip_tcp  # noqa: E402

BRIDGE = os.path.join(HERE, "..", "bridge", "tinymcp.py")
PORT = 7000
KEY = "tcp-test-key-7"
CLIENT = "board-tcp"
KEYS = ["tcp-listen", "tcp-one", "tcp-read", "tcp-long", "tcp-readonly",
        "tcp-secret", "tcp-update", "tcp-keyed", "tcp-wrong-key",
        "tcp-legacy-key", "tcp-consent", "tcp-lease"]

failures = 0
checks = 0


def expect(key, cond, what):
    global failures, checks
    checks += 1
    print(f"CHECK tcp {key} {'ok' if cond else 'fail'} {what}", flush=True)
    if not cond:
        failures += 1


def text_of(r):
    return "".join(c.text for c in r.content if isinstance(c, types.TextContent))


def arenas(text):
    """The live arenas /sys/mem/reservations lists, in any tier."""
    return len([x for x in text.splitlines() if " arena " in x and "state=live" in x])


async def declined(context, params):
    return types.ElicitResult(action="decline")


def session(port, key=None, mode="auto"):
    env = dict(os.environ)
    env.pop("TINYMCP_KEY", None)
    if key:
        env["TINYMCP_KEY"] = key
    params = StdioServerParameters(command=sys.executable,
                                   args=[BRIDGE, "--tcp", f"127.0.0.1:{port}"], env=env)
    return Client(params, mode=mode, read_timeout_seconds=30, cache=None,
                  elicitation_callback=declined,
                  client_info=types.Implementation(name=CLIENT, version="1"))


async def read(c, path):
    return text_of(await c.call_tool("read", {"path": path})).strip()


async def checks_open(port, wire):
    async with session(port) as c:
        tcp = await read(c, "/sys/mcp/tcp")
        tools = sorted(t.name for t in (await c.list_tools()).tools)
        up = (await c.read_resource("tiku:///sys/uptime")).contents[0].text.strip()
        expect("tcp-read", tcp.startswith(f"lines {PORT}, connected 172.16.7.1:") and up.isdigit()
               and "read" in tools, f"{tcp!r}; uptime {up}; tools {tools}")
        whole = (await c.read_resource("tiku:///sys/vfs/manifest")).contents[0].text
        expect("tcp-long", len(whole) > 4608 and whole.endswith("\n") and "[cut]" not in whole,
               f"{len(whole)} bytes of the manifest in one read over TCP")
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        s = await c.call_tool("shell", {"line": "reboot"})
        b = await c.call_tool("basic", {"program": "10 END\n"})
        k = await c.call_tool("write", {"path": "/sys/mcp/key", "value": "x" * 12})
        led = await read(c, "/dev/led0")
        expect("tcp-readonly", all(r.is_error and "only reads" in text_of(r) for r in (w, s, b, k))
               and led == "0", f"write {text_of(w)[:40]!r}; led {led}")
        r = await c.call_tool("read", {"path": "/sys/mcp/key"})
        uris = [x.uri for x in (await c.list_resources()).resources]
        expect("tcp-secret", r.is_error and "tiku:///sys/mcp/key" not in uris,
               f"{text_of(r)[:50]!r}")
        reserved = await read(c, "/sys/mem/reservations")
        # A change made at the console reaches a subscriber over TCP.
        heard = None
        async with c.listen(resource_subscriptions=["tiku:///dev/led0"]) as sub:
            wire.type("write /dev/led0 1")
            with anyio.move_on_after(5):
                async for ev in sub:
                    if isinstance(ev, ResourceUpdated):
                        heard = ev.uri
                        break
        expect("tcp-update", heard == "tiku:///dev/led0", f"heard {heard}")
        wire.type("write /dev/led0 0")
    return arenas(reserved)


async def checks_keyed(port):
    async with session(port, key=KEY) as c:
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        led = await read(c, "/dev/led0")
        await c.call_tool("write", {"path": "/dev/led0", "value": "0"})
        j = await read(c, "/sys/mcp/journal")
        expect("tcp-keyed", not w.is_error and led == "1" and
               f"{CLIENT} write /dev/led0 1" in j, f"{text_of(w)!r}; led {led}")
        before = int(await read(c, "/sys/uptime"))
        r = await c.call_tool("shell", {"line": "reboot"})
        await anyio.sleep(1.0)
        after = int(await read(c, "/sys/uptime"))
        expect("tcp-consent", r.is_error and after >= before,
               f"{text_of(r)[:50]!r}; uptime {before} -> {after}")
    async with session(port, key=KEY + "x") as c:
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        expect("tcp-wrong-key", w.is_error and "only reads" in text_of(w), f"{text_of(w)[:40]!r}")
    async with session(port, key=KEY, mode="legacy") as c:
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "0"})
        expect("tcp-legacy-key", c.protocol_version == "2025-11-25" and not w.is_error,
               f"{c.protocol_version}: {text_of(w)!r}")


def one_at_a_time(wire):
    """A second connection while one is open is refused."""
    a = slip_tcp.SlipTcp(wire, PORT)
    b = slip_tcp.SlipTcp(wire, PORT)
    ok_a = a.connect()
    b.connect(timeout=3)
    # The board completes the handshake, then refuses the connection.
    end = time.monotonic() + 3
    while b.state == "open" and time.monotonic() < end:
        b.turn(0.1)
    expect("tcp-one", ok_a and a.state == "open" and b.state == "reset",
           f"first {a.state}, second {b.state}")
    a.close()
    time.sleep(0.5)
    a.turn(0.2)


async def main(port_name):
    ser = serial.Serial(port_name, 115200, timeout=0)
    wire = slip_tcp.Wire(ser)
    console = bytearray()

    def drain(secs):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            console.extend(ser.read(4096))
            time.sleep(0.05)

    wire.type("")
    drain(0.5)
    # slip toggles: once more if this turned it off.
    wire.type("slip")
    drain(0.8)
    if "SLIP off" in console.decode("latin-1").splitlines()[-2:][0] if console else False:
        wire.type("slip")
        drain(0.8)
    wire.type(f"write /sys/mcp/key {KEY}")
    drain(0.5)
    wire.type(f"mcp listen {PORT}")
    drain(0.8)
    said = console.decode("latin-1")
    expect("tcp-listen", f"lines {PORT}\r\n" in said or f"lines {PORT}\n" in said,
           said.strip().splitlines()[-4:])
    one_at_a_time(wire)

    stop = threading.Event()
    ready = []
    t = threading.Thread(target=slip_tcp.relay, args=(wire, PORT, stop, ready), daemon=True)
    t.start()
    while not ready:
        time.sleep(0.05)
    try:
        during = await checks_open(ready[0], wire)
        await checks_keyed(ready[0])
        # A new connection after those closed: its buffers came back.
        async with session(ready[0]) as c:
            await anyio.sleep(0.5)
            tcp = await read(c, "/sys/mcp/tcp")
            again = arenas(await read(c, "/sys/mem/reservations"))
        expect("tcp-lease", during == again and ", connected " in tcp,
               f"live arenas during the first connection {during}, during a later one {again}")
    finally:
        stop.set()
        t.join(2)
        wire.type(f"mcp listen off")
        wire.type("write /sys/mcp/key none")
        time.sleep(0.5)
        ser.close()
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: board_tcp.py <serial port>")
        sys.exit(2)
    sys.exit(anyio.run(main, sys.argv[1]))
