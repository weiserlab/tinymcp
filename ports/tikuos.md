# tinymcp on tikuOS

tikuOS is the first host.  Its binding is not in this repository: it
lives in the tikuOS tree as `services/tinymcp/`, built with
`TIKU_MCP_ENABLE=1`, and takes this library from a sibling checkout
(`tinymcp/` beside `tikukits/`, gitignored there).  This page records
what the binding maps, so another host can take the same shape.

## What the host table holds

| Entry | tikuOS |
|---|---|
| `name`, `title`, `version` | `/sys/device/name` read before each request, "tikuOS on <device>", `TIKU_VERSION` |
| `instructions` | the board's identity: port, board, what the namespace is |
| `tools` | `read`, `write`, `ls` over the VFS; `shell` and `basic` where the shell is built |
| `templates` | `tiku:///{+path}` and `tiku:///{+path}{?line,bytes}` |
| `resource_list` | `/sys/vfs/manifest` paged by line, `/data` files from `tiku_vfs_list` |
| `resource_read` | `tiku_vfs_read_policy`, so a node whose read acts is `TINYMCP_EEFFECT` |
| `tool_call` | the VFS for `read`/`write`/`ls`, a typed node's value as `structuredContent {value, unit}` decoded from the text read; the shell parser with its output captured through `tinymcp_sink_putc` for `shell`; the BASIC job API for `basic`, a call kept with `tinymcp_defer` |
| `watch`, `unwatch` | `tiku_vfs_watch` into the tinymcp process; `TIKU_EVENT_VFS` becomes `tinymcp_updated(slot)` |
| `prompt_list`, `prompt_get` | `/data/prompt.<name>` files: the first line describes, `{{argument}}` places are the arguments, `tinymcp_render` fills them |
| `complete` | a `path` argument against the namespace, through `tiku_vfs_list` |
| `cancel` | stops the job of the cancelled `basic` call |
| `send` | one console frame on channel `0xF3`, or `tcp_send` from the pump |
| `send_part` | one console frame begun at the first piece and ended at the last, the watchdog kicked at each |

## The board asks

`/sys/mcp/ask/` holds one question at a time: `question` (written by the
`ask` shell command, a rule whose action is `ask ...`, BASIC's
`ASK q$, A$[, seconds]`, or any writer), `answer` (written by a client;
a leading `#<seq> ` names the question it answers, and an answer to an
older one is refused), `seq`, `state` (`idle`, `asked`, `answered`),
`asked_by` and `answered_by`.  A writer is the client's name from
`tinymcp_client()` while one of its tools runs, `board` otherwise.
Before an agent reboots the board, changes its sleep mode, powers a part
down, changes the CPU clock or the memory layout, through the `write` or
the `shell` tool or a job or rule that would, the binding asks the owner:
a client that can elicit gets an `input_required` form, any other a token
in the error, which it writes to `/sys/mcp/consent` before it makes the
same call again within 60 s.

`/sys/mcp/journal` is a RAM ring of the last 16 entries, `<seconds> <who>
<what>`: questions, answers, the `write` and `shell` tools, and whatever a
client writes there.

## Jobs

The `basic` tool runs a BASIC program, sent as numbered lines, as the
board's one job: a headless run of the BASIC mode whose output goes to a
1 KB ring.  The call is kept (`tinymcp_defer`) and answered when the job
ends, with the output; each line printed goes out as
`notifications/progress`, at most one every 250 ms, and the job is
stopped after the call's `seconds` (60).  With `background: true` the call
answers at once and `/sys/mcp/job/` carries the job: `seq`, `state`
(`idle`, `running`, `done`, `error`, `stopped`), `by`, `out` (the ring,
subscribable) and `stop` (any write ends it).  A cancelled call stops its
job.  A job has no console and no system capability: `INPUT`, `REBOOT`
and `POKE` are errors in it, and a write to a node that needs
`TIKU_VFS_CAP_SYS` (the clock target, power, the layout, the watchdog) is
`EPERM`.

## Streams

One `tinymcp_t` per stream: the console channel, and one TCP connection
at a time from `mcp listen`.  The console channel's callback copies the
frame and marks it pending; the request runs from the shell's pump
between command lines, never inside the callback.  The host bridge
(`bridge/tinymcp.py`) turns a client's stdio into those frames.

## Buffers

| Port class | in | out |
|---|---|---|
| Cortex-M (nRF54L, RP2350, Ambiq, RA8P1, STM32N6), ESP32-C5 | 1024 | 4608 |
| ESP32-C61, ESP32-P4 | 2048 | 8192, in PSRAM |
| MSP430FR5994 | 256 | 512 |

## Authority

The console channel carries the owner's capability (every write
allowed).  A TCP stream opened by `mcp listen <port>` carries none:
`write`, `shell` and `basic` answer `isError` "read-only carrier" until
the owner typed `mcp listen <port> all` at the console or placed it in
the init table.
