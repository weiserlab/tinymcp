#!/usr/bin/env python3
#
# tinymcp v0.7
# The Model Context Protocol server as a library any firmware can carry.
# http://tinymcp.org
#
# Authors: Ambuj Varshney <ambuj@tiku-os.org>
#
# board_smoke.py - the official mcp SDK client against a tikuOS board
# through the bridge, in both eras: the tools, resources, prompts, asks,
# the owner's consent and BASIC jobs, one "CHECK <era> <key> ok|fail
# <detail>" line per check, which TikuBench's tinymcp suite reads.
#
# SPDX-License-Identifier: Apache-2.0

import os
import re
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
        "complete", "complete-root", "ask-heard", "answer", "answer-stale",
        "journal", "consent-no", "consent-yes", "consent-hidden",
        "consent-journal", "job-run", "job-progress", "job-meanwhile", "job-background",
        "job-one", "job-stop", "job-cancel", "job-budget", "job-confined",
        "job-journal", "stream-read", "stream-tool", "structured"]
CLIENT = "board-smoke"

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
                      read_timeout_seconds=20, cache=None,
                      elicitation_callback=on_elicit,
                      client_info=types.Implementation(name=CLIENT, version="1")) as c:
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
        expect("tools", names in (["ls", "read", "shell", "write"],
                                  ["basic", "ls", "read", "shell", "write"]),
               f"tools {names}")

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
        await streamed(c)
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
        await asks(c, mode, seen)
        await consent(c, mode)
        if "basic" in names:
            await jobs(c)
        else:
            for k in KEYS:
                if k.startswith("job-"):
                    expect(k, False, "the board has no basic tool")
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


ANSWER = {"action": "decline", "asked": None}


async def on_elicit(context, params):
    """The owner, as the test plays them: ANSWER["action"] to every form."""
    ANSWER["asked"] = params.message
    if ANSWER["action"] == "accept":
        return types.ElicitResult(action="accept", content={"confirm": True})
    return types.ElicitResult(action=ANSWER["action"])


async def uptime(c):
    return int((await c.read_resource("tiku:///sys/uptime")).contents[0].text.strip())


def token_of(text):
    m = re.search(r"write (\d+) to /sys/mcp/consent", text)
    return m.group(1) if m else None


async def consent(c, mode):
    """A reboot refused, a clock target allowed, a job that would reboot."""
    legacy = mode == "legacy"
    before = await uptime(c)
    ANSWER.update(action="decline", asked=None)
    try:
        r = await c.call_tool("shell", {"line": "reboot"})
    except MCPError as e:
        expect("consent-no", False, f"the board did not answer, so it rebooted: {e}")
        return
    await anyio.sleep(1.5)
    after = await uptime(c)
    if legacy:
        ok = r.is_error and token_of(text_of(r)) and ANSWER["asked"] is None
    else:
        ok = r.is_error and ANSWER["asked"] == f"{CLIENT} asks to reboot the board.  Allow it?"
    expect("consent-no", ok and after >= before,
           f"{text_of(r)[:70]!r}; uptime {before} -> {after}")

    target = (await c.read_resource("tiku:///sys/cpu/freq_target")).contents[0].text.strip()
    ANSWER.update(action="accept", asked=None)
    r = await c.call_tool("write", {"path": "/sys/cpu/freq_target", "value": target})
    detail = text_of(r)[:60]
    if legacy:
        token = token_of(text_of(r))
        bad = await c.call_tool("write", {"path": "/sys/mcp/consent", "value": "1"})
        good = await c.call_tool("write", {"path": "/sys/mcp/consent", "value": token or "0"})
        r = await c.call_tool("write", {"path": "/sys/cpu/freq_target", "value": target})
        ok = token and bad.is_error and not good.is_error and not r.is_error
        detail = f"token {token}; wrong {text_of(bad)!r}; again {text_of(r)!r}"
    else:
        ok = not r.is_error and "change the CPU clock" in (ANSWER["asked"] or "")
    expect("consent-yes", ok, f"target {target}: {detail}")

    ANSWER.update(action="decline", asked=None)
    r = await c.call_tool("shell", {"line": "once 300 reboot"})
    jobs = text_of(await c.call_tool("shell", {"line": "jobs"}))
    expect("consent-hidden", r.is_error and "reboot" not in jobs,
           f"{text_of(r)[:50]!r}; jobs {jobs.strip()!r}")

    j = (await c.read_resource("tiku:///sys/mcp/journal")).contents[0].text
    expect("consent-journal", f"{CLIENT} allowed: change the CPU clock" in j and
           f"{CLIENT} asked consent: reboot the board" in j,
           " | ".join(j.splitlines()[-4:]))


OUT_CAP = 4608   # the board's reply buffer on a Cortex-M


async def streamed(c):
    """Past the reply buffer in one request; a typed node's value."""
    whole = (await c.read_resource("tiku:///sys/vfs/manifest")).contents[0].text
    head = (await c.read_resource("tiku:///sys/vfs/manifest?line=0&bytes=300")).contents[0].text
    lines = whole.splitlines()
    expect("stream-read", len(whole) > OUT_CAP and "[cut]" not in whole and
           whole.startswith(head) and whole.endswith("\n") and
           all(line.count("\t") >= 3 for line in lines[1:]),
           f"{len(whole)} bytes, {len(lines)} lines in one resources/read")
    r = await c.call_tool("read", {"path": "/sys/vfs/manifest"})
    expect("stream-tool", not r.is_error and text_of(r) == whole,
           f"{len(text_of(r))} bytes from the read tool")
    r = await c.call_tool("read", {"path": "/sys/cpu/freq"})
    plain = await c.call_tool("read", {"path": "/sys/device/name"})
    sc = r.structured_content or {}
    expect("structured", sc.get("unit") == "Hz" and sc.get("value") == int(text_of(r)) and
           plain.structured_content is None,
           f"{text_of(r).strip()!r} -> {sc}; name {plain.structured_content}")


async def node(c, path):
    return (await c.read_resource("tiku://" + path)).contents[0].text.strip()


async def job_ends(c, seconds):
    """The job's state once it is no longer running, or "running"."""
    state = "running"
    with anyio.move_on_after(seconds):
        while state == "running":
            await anyio.sleep(0.3)
            state = await node(c, "/sys/mcp/job/state")
    return state


COUNT = ('10 FOR I = 1 TO 4\n20 PRINT "LINE" + STR$(I)\n30 DELAY 400\n'
         '40 NEXT I\n50 PRINT "END"\n')


async def jobs(c):
    """BASIC programs as the board's job, waited for and in the background."""
    notes = []

    async def on_progress(progress, total, message):
        notes.append((progress, message))

    r = await c.call_tool("basic", {"program": COUNT}, progress_callback=on_progress)
    out = text_of(r)
    expect("job-run", not r.is_error and "LINE4" in out and out.rstrip().endswith("END"),
           f"{out.strip()!r}")
    expect("job-progress", len(notes) >= 2 and all(m and m.startswith("LINE") for _, m in notes)
           and [p for p, _ in notes] == sorted(p for p, _ in notes),
           f"progress {notes}")

    # While the call waits, the board answers other requests.
    got = {}

    async def waited():
        got["out"] = text_of(await c.call_tool("basic", {"program": COUNT}))

    async with anyio.create_task_group() as tg:
        tg.start_soon(waited)
        await anyio.sleep(0.6)
        got["state"] = await node(c, "/sys/mcp/job/state")
        got["read"] = text_of(await c.call_tool("read", {"path": "/sys/uptime"})).strip()
        got["early"] = "out" not in got
    expect("job-meanwhile", got["early"] and got["state"] == "running" and
           got["read"].isdigit() and "END" in got.get("out", ""),
           f"{got}")

    # In the background: the call answers at once, other reads go on, the
    # output and the end are nodes; a second job is refused while it runs.
    r = await c.call_tool("basic", {"program": COUNT, "background": True})
    started = text_of(r)
    seq = await node(c, "/sys/mcp/job/seq")
    up = await uptime(c)
    running = await node(c, "/sys/mcp/job/state")
    second = await c.call_tool("basic", {"program": "10 END\n"})
    state = await job_ends(c, 10)
    out = await node(c, "/sys/mcp/job/out")
    expect("job-background", not r.is_error and f"job #{seq} runs" in started and
           up >= 0 and running == "running" and state == "done" and "LINE4" in out and
           await node(c, "/sys/mcp/job/by") == CLIENT,
           f"{started[:30]!r}; {running} -> {state}; out {out[-20:]!r}")
    expect("job-one", second.is_error and "is running" in text_of(second),
           f"{text_of(second)[:60]!r}")

    # Stopped through its node; cancelled by the client; past its seconds.
    await c.call_tool("basic", {"program": '10 PRINT "S"\n20 DELAY 100000\n',
                                "background": True})
    await anyio.sleep(0.5)
    r = await c.call_tool("write", {"path": "/sys/mcp/job/stop", "value": "1"})
    state = await job_ends(c, 3)
    out = await node(c, "/sys/mcp/job/out")
    again = await c.call_tool("write", {"path": "/sys/mcp/job/stop", "value": "1"})
    expect("job-stop", not r.is_error and state == "stopped" and
           "stopped at line 20" in out and again.is_error,
           f"{state}; out {out!r}; again {text_of(again)!r}")

    with anyio.move_on_after(1.5):
        await c.call_tool("basic", {"program": "10 DELAY 100000\n"})
    state = await job_ends(c, 3)
    j = await node(c, "/sys/mcp/journal")
    seq = await node(c, "/sys/mcp/job/seq")
    expect("job-cancel", state == "stopped" and f"{CLIENT} job #{seq}: cancelled" in j,
           f"{state}; journal {j.splitlines()[-1:]}")

    r = await c.call_tool("basic", {"program": "10 DELAY 100000\n", "seconds": 2})
    expect("job-budget", r.is_error and "2 s ran out" in text_of(r) and
           await node(c, "/sys/mcp/job/state") == "stopped",
           f"{text_of(r)[-60:]!r}")

    # A job has no authority over the system's nodes and cannot reboot.
    target = await node(c, "/sys/cpu/freq_target")
    before = await uptime(c)
    r1 = await c.call_tool("basic", {"program": f'10 VFSWRITE$ "/sys/cpu/freq_target", "{target}"\n'})
    try:
        r2 = await c.call_tool("basic", {"program": "10 REBOOT\n"})
    except MCPError as e:
        expect("job-confined", False, f"the board did not answer, so it rebooted: {e}")
        return
    await anyio.sleep(1.0)
    after = await uptime(c)
    expect("job-confined", r1.is_error and "EPERM" in text_of(r1) and r2.is_error and
           "REBOOT is not allowed in a job" in text_of(r2) and after >= before,
           f"{text_of(r1).strip()[-40:]!r}; {text_of(r2).strip()[-40:]!r}; "
           f"uptime {before} -> {after}")

    j = await node(c, "/sys/mcp/journal")
    expect("job-journal", f"{CLIENT} job #" in j and ": started" in j and
           "board job #" in j and ": done" in j and ": stopped" in j,
           " | ".join(j.splitlines()[-5:]))


QUESTION = "tiku:///sys/mcp/ask/question"


async def asks(c, mode, seen):
    """The board asks through the shell's ask; the agent hears and answers."""
    async def ask_and_hear():
        r = await c.call_tool("shell", {"line": "ask Which colour, " + mode + "?"})
        return text_of(r)

    if mode == "legacy":
        await c.subscribe_resource(QUESTION)
        seen.clear()
        said = await ask_and_hear()
        await anyio.sleep(0.5)
        heard = QUESTION in seen
        await c.unsubscribe_resource(QUESTION)
    else:
        async with c.listen(resource_subscriptions=[QUESTION]) as sub:
            said = await ask_and_hear()
            try:
                with anyio.fail_after(5):
                    ev = await sub.__anext__()
            except TimeoutError:
                ev = None
            heard = isinstance(ev, ResourceUpdated) and ev.uri == QUESTION
    q = (await c.read_resource(QUESTION)).contents[0].text.strip()
    expect("ask-heard", heard and said.startswith("asked #") and
           q == "Which colour, " + mode + "?", f"{said.strip()!r}; question {q!r}")

    seq = said.strip()[len("asked #"):]
    r = await c.call_tool("write", {"path": "/sys/mcp/ask/answer", "value": f"#{seq} blue"})
    state = (await c.read_resource("tiku:///sys/mcp/ask/state")).contents[0].text.strip()
    who = (await c.read_resource("tiku:///sys/mcp/ask/answered_by")).contents[0].text.strip()
    expect("answer", not r.is_error and state == "answered" and who == CLIENT,
           f"state {state!r}, answered_by {who!r}")
    r1 = await c.call_tool("write", {"path": "/sys/mcp/ask/answer", "value": "again"})
    r2 = await c.call_tool("write", {"path": "/sys/mcp/ask/answer", "value": "#0 old"})
    expect("answer-stale", r1.is_error and r2.is_error and "EINVAL" in text_of(r2),
           f"a second answer {text_of(r1)!r}; an old question's {text_of(r2)!r}")
    j = (await c.read_resource("tiku:///sys/mcp/journal")).contents[0].text
    expect("journal", f"{CLIENT} asked #{seq}: Which colour" in j and
           f"{CLIENT} answered #{seq}: blue" in j,
           " | ".join(j.splitlines()[-3:]))


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
