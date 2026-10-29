#!/usr/bin/env python3
#
# tinymcp v0.8
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# board_http.py - the official mcp SDK's Streamable HTTP client against a
# tikuOS board's `mcp http` port, with no bridge: dialled at the board's
# address on the LAN, or through the slip_tcp relay carrying TCP over its
# console line.  The key travels as a bearer token.
#
# SPDX-License-Identifier: Apache-2.0

import http.client
import json
import os
import sys
import threading
import time

import anyio
import mcp_types as types
import serial
from mcp import Client
from mcp.client.streamable_http import streamable_http_client
from mcp.shared._httpx_utils import create_mcp_http_client
from mcp.shared.subscriptions import ResourceUpdated

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import slip_tcp  # noqa: E402

PORT = 8080
KEY = "http-test-key-8"
CLIENT = "board-http"
KEYS = ["http-listen", "http-read", "http-long", "http-readonly", "http-update",
        "http-keyed", "http-wrong-key", "http-progress", "http-cancel",
        "http-consent", "http-wire", "http-lease"]

failures = 0
checks = 0


def expect(key, cond, what):
    global failures, checks
    checks += 1
    print(f"CHECK http {key} {'ok' if cond else 'fail'} {what}", flush=True)
    if not cond:
        failures += 1


def text_of(r):
    return "".join(c.text for c in r.content if isinstance(c, types.TextContent))


def arenas(text):
    """The live arenas /sys/mem/reservations lists, in any tier."""
    return len([x for x in text.splitlines() if " arena " in x and "state=live" in x])


async def declined(context, params):
    return types.ElicitResult(action="decline")


class Session:
    """An SDK client over HTTP to the relay, with a bearer token or none."""

    def __init__(self, at, key=None):
        self.at = at
        self.key = key

    async def __aenter__(self):
        headers = {"Authorization": f"Bearer {self.key}"} if self.key else {}
        self.http = create_mcp_http_client(headers=headers)
        await self.http.__aenter__()
        transport = streamable_http_client(f"http://{self.at[0]}:{self.at[1]}/mcp",
                                           http_client=self.http)
        self.client = Client(transport, mode="2026-07-28", read_timeout_seconds=60,
                             cache=None, elicitation_callback=declined,
                             client_info=types.Implementation(name=CLIENT, version="1"))
        return await self.client.__aenter__()

    async def __aexit__(self, *exc):
        await self.client.__aexit__(*exc)
        await self.http.__aexit__(*exc)


async def read(c, path):
    return text_of(await c.call_tool("read", {"path": path})).strip()


def raw(at, headers, method="POST", body=b"{}"):
    c = http.client.HTTPConnection(at[0], at[1], timeout=20)
    c.request(method, "/mcp", body=body, headers=headers)
    st = c.getresponse().status
    c.close()
    return st


COUNT = ('10 FOR I = 1 TO 3\n20 PRINT "LINE" + STR$(I)\n30 DELAY 400\n'
         '40 NEXT I\n')


async def basic_once(at):
    """BASIC makes its arena on first use and keeps it: run once before the
    arenas are counted, so the counts hold only what connections leave."""
    async with Session(at, key=KEY) as c:
        await c.call_tool("basic", {"program": "10 PRINT 1\n"})


def open_conns(tcp):
    """The HTTP connections /sys/mcp/tcp says are open."""
    for line in tcp.splitlines():
        if line.startswith("http ") and " of " in line:
            return int(line.split(", ")[1].split()[0])
    return 0


async def checks_open(at, wire):
    async with Session(at) as c:
        tcp = await read(c, "/sys/mcp/tcp")
        up = (await c.read_resource("tiku:///sys/uptime")).contents[0].text.strip()
        expect("http-read", f"http {PORT} /mcp, 1 of 2 connected" in tcp and up.isdigit(),
               f"{tcp.splitlines()[1:2]}; uptime {up}")
        whole = (await c.read_resource("tiku:///sys/vfs/manifest")).contents[0].text
        expect("http-long", len(whole) > 4608 and whole.endswith("\n") and "[cut]" not in whole,
               f"{len(whole)} bytes of the manifest in chunks")
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        led = await read(c, "/dev/led0")
        expect("http-readonly", w.is_error and "only reads" in text_of(w) and led == "0",
               f"{text_of(w)[:40]!r}; led {led}")
        reserved = await read(c, "/sys/mem/reservations")
        mine = open_conns(await read(c, "/sys/mcp/tcp"))
        heard = None
        async with c.listen(resource_subscriptions=["tiku:///dev/led0"]) as sub:
            wire.type("write /dev/led0 1")
            with anyio.move_on_after(8):
                async for ev in sub:
                    if isinstance(ev, ResourceUpdated):
                        heard = ev.uri
                        break
        expect("http-update", heard == "tiku:///dev/led0", f"heard {heard} on the event stream")
        wire.type("write /dev/led0 0")
    return arenas(reserved) - mine


async def checks_keyed(at):
    async with Session(at, key=KEY) as c:
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        led = await read(c, "/dev/led0")
        await c.call_tool("write", {"path": "/dev/led0", "value": "0"})
        j = await read(c, "/sys/mcp/journal")
        expect("http-keyed", not w.is_error and led == "1" and f"{CLIENT} write /dev/led0 1" in j,
               f"{text_of(w)!r}; led {led}")
        notes = []

        async def on_progress(progress, total, message):
            notes.append(message)

        r = await c.call_tool("basic", {"program": COUNT}, progress_callback=on_progress)
        expect("http-progress", not r.is_error and "LINE3" in text_of(r) and len(notes) >= 2,
               f"progress {notes}; result {text_of(r).strip()!r}")
        # A waited-for job, cancelled once it runs: its POST is closed.
        scope = anyio.CancelScope()
        seen = []

        async def waited():
            with scope:
                await c.call_tool("basic", {"program": "10 DELAY 100000\n"})

        async with anyio.create_task_group() as tg:
            tg.start_soon(waited)
            with anyio.move_on_after(20):
                while not seen or seen[-1] != "running":
                    await anyio.sleep(0.5)
                    seen.append(await read(c, "/sys/mcp/job/state"))
            scope.cancel()
        state = seen[-1] if seen else "none"
        with anyio.move_on_after(8):
            while state == "running":
                await anyio.sleep(0.5)
                state = await read(c, "/sys/mcp/job/state")
        expect("http-cancel", "running" in seen and state == "stopped",
               f"the job ran, its POST closed: {state}")
        before = int(await read(c, "/sys/uptime"))
        r = await c.call_tool("shell", {"line": "reboot"})
        await anyio.sleep(1.0)
        after = int(await read(c, "/sys/uptime"))
        expect("http-consent", r.is_error and after >= before,
               f"{text_of(r)[:40]!r}; uptime {before} -> {after}")
    async with Session(at, key=KEY + "x") as c:
        w = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        expect("http-wrong-key", w.is_error and "only reads" in text_of(w), f"{text_of(w)[:40]!r}")


def slip_on(wire, console, drain):
    """slip toggles: once more if typing it turned it off."""
    wire.type("slip")
    drain(0.8)
    if "SLIP off" in console.decode("latin-1")[-200:]:
        wire.type("slip")
        drain(0.8)


async def main(port_name, address=None):
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
    if not address:
        slip_on(wire, console, drain)
    wire.type(f"write /sys/mcp/key {KEY}")
    drain(0.5)
    wire.type(f"mcp http {PORT}")
    drain(0.8)
    said = console.decode("latin-1")
    expect("http-listen", f"http {PORT} /mcp, 0 of 2 connected" in said,
           said.strip().splitlines()[-4:])

    stop = threading.Event()
    ready = []
    def soak():
        """On the LAN the console carries only typed lines and their output."""
        while not stop.is_set():
            drain(0.5)

    if address:
        ready.append((address, PORT))
        t = threading.Thread(target=soak, daemon=True)
    else:
        t = threading.Thread(target=slip_tcp.relay, args=(wire, PORT, stop, ready),
                             daemon=True)
    t.start()
    while not ready:
        time.sleep(0.05)
    at = ready[0] if address else ("127.0.0.1", ready[0])
    try:
        await basic_once(at)
        during = await checks_open(at, wire)
        await checks_keyed(at)
        hs = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
        got = (raw(at, dict(hs, Origin="http://evil.example")), raw(at, hs, method="GET"))
        expect("http-wire", got == (403, 405), f"a web origin and a GET: {got}")
        # Arenas beside the connections' own: the same in a later session.
        async with Session(at) as c:
            await anyio.sleep(1.0)
            reserved = await read(c, "/sys/mem/reservations")
            mine = open_conns(await read(c, "/sys/mcp/tcp"))
        again = arenas(reserved) - mine
        expect("http-lease", during == again,
               f"other live arenas: {during} in the first session, {again} later")
    finally:
        stop.set()
        t.join(5)
        wire.type("mcp http off")
        wire.type("write /sys/mcp/key none")
        time.sleep(0.5)
        ser.close()
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) not in (2, 3):
        print("usage: board_http.py <serial port> [<board address on the LAN>]")
        sys.exit(2)
    sys.exit(anyio.run(main, *sys.argv[1:]))
