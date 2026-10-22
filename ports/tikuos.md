# tinymcp on tikuOS

tikuOS is the first host.  Its binding is not in this repository: it
lives in the tikuOS tree as `services/tinymcp/`, built with
`TIKU_MCP_ENABLE=1`, and takes this library from a sibling checkout
(`tinymcp/` beside `tikukits/`, gitignored there).  This page records
what the binding maps, so another host can take the same shape.

## What the host table holds

| Entry | tikuOS |
|---|---|
| `name`, `version` | `tinymcp`, `TINYMCP_VERSION` |
| `instructions` | the board's identity: port, board, what the namespace is |
| `tools` | `read`, `write`, `ls` over the VFS; `shell` and `basic` where the shell is built |
| `templates` | `tiku:///{+path}` and `tiku:///{+path}{?line,bytes}` |
| `resource_list` | `/sys/vfs/manifest` paged by line, `/data` files from `tiku_vfs_list` |
| `resource_read` | `tiku_vfs_read_policy`, so a node whose read acts is `TINYMCP_EEFFECT` |
| `tool_call` | the VFS for `read`/`write`/`ls`; the shell parser with its output captured through `tinymcp_sink_putc` for `shell`; the BASIC mode API for `basic` |
| `watch`, `unwatch` | `tiku_vfs_watch` into the tinymcp process; `TIKU_EVENT_VFS` becomes `tinymcp_updated(slot)` |
| `send` | one console frame on channel `0xF3`, or `tcp_send` from the pump |

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
