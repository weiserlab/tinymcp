#!/usr/bin/env python3
#
# tinymcp v0.2
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# conformance.py - the official mcp SDK client against the POSIX example in
# every era it can speak: pinned 2026-07-28, auto-negotiated, and the
# initialize handshake.  Each run lists, calls, reads, subscribes and sees a
# change arrive, gets a prompt and completes a name.
#
# SPDX-License-Identifier: Apache-2.0

import asyncio
import os
import sys
import tempfile

import anyio
import mcp_types as types
from mcp import Client, MCPError
from mcp.client.stdio import StdioServerParameters
from mcp.shared.subscriptions import (PromptsListChanged, ResourcesListChanged,
                                      ResourceUpdated)

failures = 0
checks = 0


def expect(cond, what):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL: {what}")


def text_of(result):
    return "".join(c.text for c in result.content if isinstance(c, types.TextContent))


async def common(c, mode, dirpath):
    legacy = mode == "legacy"
    if mode == "legacy" or mode == "auto":
        expect(c.server_info is not None and c.server_info.name == "tinymcp-posix",
               f"serverInfo name: {c.server_info}")
        expect(c.server_info is not None and c.server_info.version == "0.2.0",
               "serverInfo version")
        expect(c.server_info is not None and c.server_info.title == "tinymcp on POSIX",
               f"serverInfo title: {c.server_info}")
        instructions = c.instructions
        caps = c.server_capabilities
    else:
        # A pinned version adopts without a discover; send one by hand.
        d = types.DiscoverResult.model_validate(await c.session.send_discover("2026-07-28"))
        expect("2026-07-28" in d.supported_versions and "2025-11-25" in d.supported_versions,
               f"supported versions {d.supported_versions}")
        expect(d.ttl_ms == 3600000 and d.cache_scope == "private", "discover cache hints")
        instructions = d.instructions
        caps = d.capabilities
    expect(instructions and "resources" in instructions, "instructions present")
    expect(caps.tools is not None, "tools capability")
    expect(caps.resources is not None and caps.resources.subscribe is True
           and caps.resources.list_changed is True, f"resources capability: {caps.resources}")
    expect(caps.prompts is not None and caps.prompts.list_changed is True,
           f"prompts capability: {caps.prompts}")
    expect(caps.completions is not None, "completions capability")

    tools = await c.list_tools()
    names = sorted(t.name for t in tools.tools)
    expect(names == ["echo", "shell", "write"], f"tool names {names}")
    echo = next(t for t in tools.tools if t.name == "echo")
    expect(echo.input_schema.get("required") == ["text"], "echo schema kept verbatim")

    sample = 'héllo "q" \\ tab\t nl\n 😀 \x01'
    r = await c.call_tool("echo", {"text": sample})
    expect(not r.is_error and text_of(r) == sample, f"echo round trip: {text_of(r)!r}")

    r = await c.call_tool("shell", {"line": "echo hi; echo there >/dev/null"})
    expect(not r.is_error and text_of(r) == "hi\n", f"shell output {text_of(r)!r}")
    r = await c.call_tool("shell", {"line": "exit 3"})
    expect(r.is_error is True, "a failing command is a tool error")
    r = await c.call_tool("shell", {"line": "head -c 40000 /dev/zero | tr '\\0' x"})
    expect(r.is_error is True and text_of(r).endswith("[cut]") and len(text_of(r)) < 33000,
           f"a long output is cut: {len(text_of(r))} bytes, isError {r.is_error}")

    try:
        await c.call_tool("nope", {})
        expect(False, "unknown tool accepted")
    except MCPError as e:
        expect(e.error.code == -32602, f"unknown tool code {e.error.code}")
    try:
        await c.call_tool("echo", {"text": 5})
        expect(False, "wrong argument type accepted")
    except MCPError as e:
        expect(e.error.code == -32602, f"wrong argument code {e.error.code}")

    res = await c.list_resources()
    uris = sorted(x.uri for x in res.resources)
    expect(uris == ["file:///a.txt", "file:///b.txt"], f"resources {uris}")
    expect(all(x.mime_type == "text/plain" for x in res.resources), "mimeType text/plain")
    tpl = await c.list_resource_templates()
    expect(len(tpl.resource_templates) == 2 and
           tpl.resource_templates[0].uri_template == "file:///{name}", "templates")

    r = await c.read_resource("file:///a.txt")
    expect(r.contents[0].text == "one\ntwo\nthree\n", f"read whole {r.contents[0].text!r}")
    expect(r.contents[0].uri == "file:///a.txt", "read echoes the uri")
    r = await c.read_resource("file:///a.txt?line=1&bytes=3")
    expect(r.contents[0].text == "two", f"read a page {r.contents[0].text!r}")
    expect(r.contents[0].uri == "file:///a.txt?line=1&bytes=3", "page echoes the query")
    try:
        await c.read_resource("file:///none.txt")
        expect(False, "unknown resource read")
    except MCPError as e:
        want = -32002 if legacy else -32602
        expect(e.error.code == want, f"unknown resource code {e.error.code}, want {want}")
    try:
        await c.read_resource("file:///../etc/passwd")
        expect(False, "a path outside the directory read")
    except MCPError as e:
        expect(e.error.code in (-32002, -32602), "path outside the directory refused")

    await prompts_and_completion(c)

    if legacy:
        await c.send_ping()
        expect(True, "ping answered")


async def prompts_and_completion(c):
    p = await c.list_prompts()
    expect([x.name for x in p.prompts] == ["greet"], f"prompt names {p.prompts}")
    greet = p.prompts[0]
    expect(greet.description == "Greet a person", f"prompt description {greet.description!r}")
    expect([(a.name, a.required) for a in greet.arguments or []] ==
           [("who", True), ("lang", True)], f"prompt arguments {greet.arguments}")
    r = await c.get_prompt("greet", {"who": 'Ada "L"', "lang": "C"})
    expect(len(r.messages) == 1 and r.messages[0].role == "user", "one user message")
    expect(r.messages[0].content.text == 'Greet a person\nSay hello to Ada "L" in C.\n',
           f"prompt text {r.messages[0].content.text!r}")
    for name, args in (("greet", {"who": "Ada"}), ("nobody", {})):
        try:
            await c.get_prompt(name, args)
            expect(False, f"prompts/get {name} {args} accepted")
        except MCPError as e:
            expect(e.error.code == -32602, f"prompts/get {name} code {e.error.code}")

    ref = types.ResourceTemplateReference(uri="file:///{name}")
    r = await c.complete(ref, {"name": "name", "value": "a"})
    expect(r.completion.values == ["a.txt"] and r.completion.has_more is False,
           f"completion of a {r.completion}")
    r = await c.complete(ref, {"name": "name", "value": ""})
    expect(r.completion.values == ["a.txt", "b.txt"] and r.completion.total == 2,
           f"completion of nothing {r.completion}")
    r = await c.complete(types.PromptReference(name="greet"), {"name": "who", "value": ""})
    expect(r.completion.values == [], f"completion of who {r.completion}")
    try:
        await c.complete(types.PromptReference(name="nobody"), {"name": "who", "value": ""})
        expect(False, "completion of an unknown prompt accepted")
    except MCPError as e:
        expect(e.error.code == -32602, f"unknown prompt completion code {e.error.code}")


async def modern_subscriptions(c, dirpath):
    async with c.listen(resources_list_changed=True,
                        resource_subscriptions=["file:///a.txt", "file:///zzz.txt"]) as sub:
        expect(sub.honored.resources_list_changed is True, "list changes honoured")
        expect(list(sub.honored.resource_subscriptions or []) == ["file:///a.txt"],
               f"honoured subscriptions {sub.honored.resource_subscriptions}")
        r = await c.call_tool("write", {"name": "a.txt", "text": "changed\n"})
        expect(not r.is_error, "write a.txt")
        with anyio.fail_after(5):
            ev = await sub.__anext__()
        expect(isinstance(ev, ResourceUpdated) and ev.uri == "file:///a.txt",
               f"updated event {ev}")
        r = await c.call_tool("write", {"name": "c.txt", "text": "new\n"})
        with anyio.fail_after(5):
            ev = await sub.__anext__()
        expect(isinstance(ev, ResourcesListChanged), f"list changed event {ev}")
        r = await c.read_resource("file:///a.txt")
        expect(r.contents[0].text == "changed\n", "the change is readable")
    async with c.listen(prompts_list_changed=True) as sub:
        expect(sub.honored.prompts_list_changed is True, "prompt list changes honoured")
        r = await c.call_tool("write", {"name": "prompt.bye", "text": "Say bye\n"})
        with anyio.fail_after(5):
            ev = await sub.__anext__()
        expect(isinstance(ev, PromptsListChanged), f"prompts changed event {ev}")
        p = await c.list_prompts()
        expect([x.name for x in p.prompts] == ["bye", "greet"], f"prompts after {p.prompts}")
    # After the stream is closed, a change raises nothing on the client.
    r = await c.call_tool("write", {"name": "a.txt", "text": "again\n"})
    expect(not r.is_error, "write after the listen ended")


async def legacy_subscriptions(c, seen):
    await c.subscribe_resource("file:///a.txt")
    r = await c.call_tool("write", {"name": "a.txt", "text": "changed\n"})
    await anyio.sleep(0.2)
    expect(("updated", "file:///a.txt") in seen, f"legacy updated notification {seen}")
    r = await c.call_tool("write", {"name": "c.txt", "text": "new\n"})
    await anyio.sleep(0.2)
    expect(("list_changed", None) in seen, f"legacy list_changed notification {seen}")
    r = await c.call_tool("write", {"name": "prompt.bye", "text": "Say bye\n"})
    await anyio.sleep(0.2)
    expect(("prompts_changed", None) in seen, f"legacy prompts list_changed {seen}")
    await c.unsubscribe_resource("file:///a.txt")
    seen.clear()
    r = await c.call_tool("write", {"name": "a.txt", "text": "silent\n"})
    await anyio.sleep(0.2)
    expect(("updated", "file:///a.txt") not in seen, f"no notification after unsubscribe {seen}")
    try:
        await c.subscribe_resource("file:///none.txt")
        expect(False, "subscribing to a missing file accepted")
    except MCPError as e:
        expect(e.error.code == -32002, f"missing subscription code {e.error.code}")


async def run(mode, exe):
    print(f"mode {mode}")
    with tempfile.TemporaryDirectory() as dirpath:
        with open(os.path.join(dirpath, "a.txt"), "w") as f:
            f.write("one\ntwo\nthree\n")
        with open(os.path.join(dirpath, "b.txt"), "w") as f:
            f.write("bravo")
        with open(os.path.join(dirpath, "prompt.greet"), "w") as f:
            f.write("Greet a person\nSay hello to {{who}} in {{lang}}.\n")
        seen = []

        async def handler(msg):
            if isinstance(msg, types.ResourceUpdatedNotification):
                seen.append(("updated", msg.params.uri))
            elif isinstance(msg, types.ResourceListChangedNotification):
                seen.append(("list_changed", None))
            elif isinstance(msg, types.PromptListChangedNotification):
                seen.append(("prompts_changed", None))
            elif isinstance(msg, Exception):
                seen.append(("error", repr(msg)))

        params = StdioServerParameters(command=exe, args=["--dir", dirpath])
        async with Client(params, mode=mode, message_handler=handler,
                          read_timeout_seconds=10, cache=None) as c:
            want = "2025-11-25" if mode == "legacy" else "2026-07-28"
            expect(c.protocol_version == want,
                   f"protocol version {c.protocol_version}, want {want}")
            await common(c, mode, dirpath)
            if mode == "legacy":
                await legacy_subscriptions(c, seen)
            else:
                await modern_subscriptions(c, dirpath)
        expect(not any(k == "error" for k, _ in seen), f"no transport errors {seen}")


async def main(exe):
    for mode in ("2026-07-28", "auto", "legacy"):
        await run(mode, exe)
    print(f"{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: conformance.py <tinymcp_posix executable>")
        sys.exit(2)
    sys.exit(anyio.run(main, os.path.abspath(sys.argv[1])))
