#!/usr/bin/env python3
#
# tinymcp v0.2
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# board_smoke.py - the official mcp SDK client against a tikuOS board
# through the bridge: discover, list, read a node, write the LED, subscribe
# and see the change, run a shell line, get a prompt the board holds and
# complete a path.  Both eras.  Every check prints one
# "CHECK <era> <key> ok|fail <detail>" line, which TikuBench's suite reads.
#
# SPDX-License-Identifier: Apache-2.0

import os
import sys

import anyio
import mcp_types as types
from mcp import Client, MCPError
from mcp.client.stdio import StdioServerParameters
from mcp.shared.subscriptions import PromptsListChanged, ResourceUpdated

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "bridge", "tinymcp.py")

KEYS = ["protocol", "identity", "tools", "read", "read-tool", "list",
        "commands", "page", "ls", "write", "led", "shell", "mode-refused",
        "write-missing", "read-missing", "subscribe", "update", "status",
        "prompts", "prompt-get", "prompt-missing", "prompts-changed",
        "complete", "complete-root"]

PROMPT = "/data/prompt.smoke"
PROMPT_TEXT = "Smoke test for {{who}}\nSay hi to {{who}} from {{where}}.\n"

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
        elif isinstance(msg, types.PromptListChangedNotification):
            seen.append("prompts")
        elif isinstance(msg, Exception):
            seen.append(repr(msg))

    params = StdioServerParameters(command=sys.executable, args=[BRIDGE, port])
    async with Client(params, mode=mode, message_handler=handler,
                      read_timeout_seconds=20, cache=None) as c:
        want = "2025-11-25" if mode == "legacy" else "2026-07-28"
        expect("protocol", c.protocol_version == want, f"protocol {c.protocol_version}")
        # serverInfo is the board: its name, the device model, the OS version.
        r = await c.call_tool("read", {"path": "/sys/device/name"})
        name = text_of(r).strip()
        si = c.server_info
        expect("identity", si is not None and si.name == name and
               (si.title or "").startswith("tikuOS on ") and si.version and
               "tikuOS" in (c.instructions or ""),
               f"serverInfo {si.name if si else None!r} {si.title if si else None!r} "
               f"{si.version if si else None!r}; board name {name!r}")
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
        await prompts(c, mode, seen)
        await completion(c)
        r = await c.call_tool("shell", {"line": "mcp"})
        status = text_of(r).strip().replace("\n", " | ")
        expect("status", "tinymcp" in status and "requests" in status, status)


async def prompts(c, mode, seen):
    """A prompt the owner wrote to /data: listed, got, refused, and heard."""
    r = await c.call_tool("write", {"path": PROMPT, "value": PROMPT_TEXT})
    p = await c.list_prompts()
    smoke = next((x for x in p.prompts if x.name == "smoke"), None)
    expect("prompts", not r.is_error and smoke is not None and
           smoke.description == "Smoke test for {{who}}" and
           [(a.name, a.required) for a in smoke.arguments or []] ==
           [("who", True), ("where", True)],
           f"{[x.name for x in p.prompts]}; smoke {smoke}")
    g = await c.get_prompt("smoke", {"who": "Ada", "where": "the board"})
    text = g.messages[0].content.text if g.messages else ""
    expect("prompt-get", text == "Smoke test for Ada\nSay hi to Ada from the board.\n",
           f"{text!r}")
    try:
        await c.get_prompt("smoke", {"who": "Ada"})
        expect("prompt-missing", False, "a missing argument accepted")
    except MCPError as e:
        expect("prompt-missing", e.error.code == -32602, f"code {e.error.code}")
    if mode == "legacy":
        seen.clear()
        await c.call_tool("shell", {"line": "rm " + PROMPT})
        await anyio.sleep(0.5)
        expect("prompts-changed", "prompts" in seen, f"notifications {seen}")
    else:
        async with c.listen(prompts_list_changed=True) as sub:
            await c.call_tool("shell", {"line": "rm " + PROMPT})
            try:
                with anyio.fail_after(5):
                    ev = await sub.__anext__()
            except TimeoutError:
                ev = None
            expect("prompts-changed", sub.honored.prompts_list_changed is True and
                   isinstance(ev, PromptsListChanged), f"event {ev}")


async def completion(c):
    """Paths complete against the namespace, through the node template."""
    ref = types.ResourceTemplateReference(uri="tiku:///{+path}")
    r = await c.complete(ref, {"name": "path", "value": "dev/le"})
    expect("complete", "dev/led0" in r.completion.values and
           all(v.startswith("dev/le") for v in r.completion.values),
           f"dev/le: {r.completion.values}")
    r = await c.complete(ref, {"name": "path", "value": ""})
    expect("complete-root", "sys/" in r.completion.values and "dev/" in r.completion.values,
           f"root: {r.completion.values}")


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
