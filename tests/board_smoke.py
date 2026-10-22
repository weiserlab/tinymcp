#!/usr/bin/env python3
#
# tinymcp v0.1
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# board_smoke.py - the official mcp SDK client against a tikuOS board
# through the bridge: discover, list, read a node, write the LED, subscribe
# and see the change, run a shell line.  Both eras.  Every check prints one
# "CHECK <era> <key> ok|fail <detail>" line, which TikuBench's suite reads.
#
# SPDX-License-Identifier: Apache-2.0

import os
import sys

import anyio
import mcp_types as types
from mcp import Client, MCPError
from mcp.client.stdio import StdioServerParameters
from mcp.shared.subscriptions import ResourceUpdated

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "bridge", "tinymcp.py")

KEYS = ["protocol", "identity", "tools", "read", "read-tool", "list",
        "commands", "page", "ls", "write", "led", "shell", "mode-refused",
        "write-missing", "read-missing", "subscribe", "update", "status"]

failures = 0
checks = 0
era = ""


def expect(key, cond, what):
    global failures, checks
    checks += 1
    print(f"CHECK {era} {key} {'ok' if cond else 'fail'} {what}")
    if not cond:
        failures += 1


def text_of(result):
    return "".join(c.text for c in result.content if isinstance(c, types.TextContent))


async def run(mode, port):
    global era
    era = "legacy" if mode == "legacy" else "modern"
    seen = []

    async def handler(msg):
        if isinstance(msg, types.ResourceUpdatedNotification):
            seen.append(msg.params.uri)
        elif isinstance(msg, Exception):
            seen.append(repr(msg))

    params = StdioServerParameters(command=sys.executable, args=[BRIDGE, port])
    async with Client(params, mode=mode, message_handler=handler,
                      read_timeout_seconds=20, cache=None) as c:
        want = "2025-11-25" if mode == "legacy" else "2026-07-28"
        expect("protocol", c.protocol_version == want, f"protocol {c.protocol_version}")
        if mode == "legacy":
            expect("identity", c.server_info.name == "tinymcp" and "tikuOS" in (c.instructions or ""),
                   f"serverInfo {c.server_info.name} {c.server_info.version}; "
                   f"instructions {(c.instructions or '')[:40]!r}")
        else:
            d = types.DiscoverResult.model_validate(await c.session.send_discover("2026-07-28"))
            expect("identity", "2026-07-28" in d.supported_versions and "tikuOS" in (d.instructions or ""),
                   f"supported {d.supported_versions[:2]}; instructions {(d.instructions or '')[:40]!r}")
        tools = await c.list_tools()
        names = sorted(t.name for t in tools.tools)
        expect("tools", names == ["ls", "read", "shell", "write"], f"tools {names}")

        r = await c.read_resource("tiku:///sys/uptime")
        expect("read", r.contents[0].text.strip().isdigit(), f"uptime {r.contents[0].text.strip()!r}")
        r = await c.call_tool("read", {"path": "/sys/device/board"})
        expect("read-tool", not r.is_error and text_of(r).strip(), f"board {text_of(r).strip()!r}")
        res = await c.list_resources()
        uris = [x.uri for x in res.resources]
        expect("list", "tiku://shell/commands" in uris and "tiku:///sys/uptime" in uris,
               f"{len(uris)} resources, cursor {res.next_cursor!r}")
        r = await c.read_resource("tiku://shell/commands")
        expect("commands", "help" in r.contents[0].text, "command table resource")
        r = await c.read_resource("tiku:///sys/vfs/manifest?line=1&bytes=200")
        expect("page", 0 < len(r.contents[0].text) <= 200, f"{len(r.contents[0].text)} bytes of the manifest")
        r = await c.call_tool("ls", {"path": "/dev"})
        expect("ls", "led0" in text_of(r), f"ls /dev: {text_of(r)[:40]!r}")
        r = await c.call_tool("write", {"path": "/dev/led0", "value": "1"})
        expect("write", not r.is_error, f"write led0 1: {text_of(r)!r}")
        r = await c.read_resource("tiku:///dev/led0")
        expect("led", r.contents[0].text.strip() == "1", f"led0 reads {r.contents[0].text.strip()!r}")
        r = await c.call_tool("shell", {"line": "info"})
        expect("shell", not r.is_error and "uptime" in text_of(r).lower(), f"info: {text_of(r)[:50]!r}")
        r = await c.call_tool("shell", {"line": "watch /dev/led0"})
        expect("mode-refused", r.is_error, f"watch refused: {text_of(r)[:50]!r}")
        r = await c.call_tool("write", {"path": "/sys/nothing", "value": "1"})
        expect("write-missing", r.is_error and "ENOENT" in text_of(r), f"{text_of(r)!r}")
        try:
            await c.read_resource("tiku:///sys/nothing")
            expect("read-missing", False, "missing resource read")
        except MCPError as e:
            want_code = -32002 if mode == "legacy" else -32602
            expect("read-missing", e.error.code == want_code, f"code {e.error.code}")

        if mode == "legacy":
            await c.subscribe_resource("tiku:///dev/led0")
            expect("subscribe", True, "resources/subscribe answered")
            await c.call_tool("write", {"path": "/dev/led0", "value": "0"})
            await anyio.sleep(0.5)
            expect("update", "tiku:///dev/led0" in seen, f"notifications {seen}")
            await c.unsubscribe_resource("tiku:///dev/led0")
        else:
            async with c.listen(resource_subscriptions=["tiku:///dev/led0"]) as sub:
                expect("subscribe", list(sub.honored.resource_subscriptions or []) == ["tiku:///dev/led0"],
                       f"honoured {sub.honored.resource_subscriptions}")
                await c.call_tool("write", {"path": "/dev/led0", "value": "0"})
                try:
                    with anyio.fail_after(5):
                        ev = await sub.__anext__()
                except TimeoutError:
                    ev = None
                expect("update", isinstance(ev, ResourceUpdated) and ev.uri == "tiku:///dev/led0",
                       f"event {ev}")
        r = await c.call_tool("shell", {"line": "mcp"})
        status = text_of(r).strip().replace("\n", " | ")
        expect("status", "tinymcp" in status and "requests" in status, status)


async def main(port, modes):
    for mode in modes:
        await run(mode, port)
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: board_smoke.py <serial port> [auto|legacy ...]")
        sys.exit(2)
    sys.exit(anyio.run(main, sys.argv[1], sys.argv[2:] or ["auto", "legacy"]))
