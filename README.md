# tinymcp

tinymcp is the Model Context Protocol (MCP) server as a library that runs
on the device: C99, two files, no allocation, no operating system calls,
so a microcontroller's firmware can carry it and an agent can read, change
and watch the device through a cable.  It is not Golioth's tinymcp, which
runs on a host and reaches the device through their cloud.

A host binds the library by filling a table (its tools, its resources, a
`send` function), owning two buffers, and feeding it bytes.  The library
answers every message of the 2026-07-28 revision of the protocol and of the
initialize-era revisions before it (2025-11-25 back to 2024-11-05), on the
same stream, and keeps the subscriptions a client opens.

tikuOS is the first host (`ports/tikuos.md`).  `examples/posix/` is a
complete host for a POSIX system and the conformance target.

## What it is

```
tinymcp.h, tinymcp.c            the server: eras, methods, errors, subscriptions
tinymcp_json.h, tinymcp_json.c  the scanner, in-place decoding, the bounded writer
examples/posix/                 a server over stdio: a directory as resources, a shell tool
bridge/tinymcp.py               an MCP client's stdio to a device's serial port or TCP port
tests/                          the protocol against a fake host; mutants; the SDK; the bridge
ports/                          what each host maps
hygiene/                        the commit and style checks
```

| Measured on 2026-10-22 | |
|---|---|
| code, `-Os`, Cortex-M33 (`make size`) | 10.9 KB (`tinymcp.c` 8.9 KB, `tinymcp_json.c` 2.1 KB) |
| code, `-Os`, Cortex-M0+ | 10.9 KB |
| one instance, `tinymcp_t`, four subscription slots | 656 bytes |
| stack during a tool call | about 1 KB (the argument table) |
| buffers | the host's: the in buffer holds one request, the out buffer one reply |

## Binding it

```c
#include "tinymcp.h"

static int my_send(void *ctx, const char *msg, size_t len);     /* one message, no newline */
static int my_tool(void *ctx, const char *name, const tinymcp_args_t *args,
                   tinymcp_sink_t *text);                         /* write the output into text */

static const tinymcp_tool_t tools[] = {
    { "blink", "Blink the LED n times",
      "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\"}}}" },
};
static const tinymcp_host_t host = {
    .name = "my-board", .version = "1.0",
    .instructions = "A board on the bench.",
    .tools = tools, .ntools = 1,
    .tool_call = my_tool, .send = my_send,
};
static char in[1024], out[4096];
static tinymcp_t server;

tinymcp_init(&server, &host, NULL, in, sizeof in, out, sizeof out, 4);
/* then, for every byte from the stream: */
tinymcp_feed(&server, bytes, n);
```

`tinymcp_feed()` takes a byte stream and splits it on newlines, which is
the protocol's stdio framing and what the specification asks of a
transport over a reliable byte stream.  A transport that delivers whole
messages calls `tinymcp_message()` instead.  Replies leave through
`send` before either returns.

Resources come from `resource_list` and `resource_read` in the table;
subscriptions from `watch` and `unwatch`, after which the host calls
`tinymcp_updated(slot)` when the watched thing changes and
`tinymcp_listchanged()` when the set of resources does.  Both may be
called from inside a tool call: the notification follows the reply.
`tinymcp_closed()` drops the stream's subscriptions and era.

A tool writes its output through `tinymcp_sink_putc()`, so a shell whose
output function takes a character can be pointed at the reply directly.
Output past the out buffer is cut, marked `[cut]`, and the result carries
`isError`.

## The protocol it serves

| Method | Era |
|---|---|
| `server/discover` | 2026-07-28 |
| `initialize`, `notifications/initialized`, `ping` | handshake eras |
| `tools/list`, `tools/call` | both |
| `resources/list`, `resources/templates/list`, `resources/read` | both |
| `subscriptions/listen`, `notifications/cancelled` | 2026-07-28 |
| `resources/subscribe`, `resources/unsubscribe` | handshake eras |

A request that carries `_meta.io.modelcontextprotocol/protocolVersion` is
served statelessly as the 2026-07-28 revision asks, with
`clientCapabilities` required and an unsupported version answered by
`-32022`; `initialize` switches the stream to the handshake era for the
requests that follow.  Text content only; no prompts, no sampling, no
HTTP.  A host with an HTTP server wraps `tinymcp_message()`.

## The bridge

Anthropic's clients speak stdio to a local process, so a device on a
serial port is reached through `bridge/tinymcp.py`: stdin lines become
frames on the console line (channel `0xF3`, SLIP-escaped between `0xC0`
bytes, the framing tikuOS consoles use), frames become stdout lines, and
the text around them goes to stderr.  `--raw` sends newline-delimited
bytes for a port that carries nothing else; `--tcp HOST:PORT` dials a
device instead.

```
claude mcp add pico2 -- python3 bridge/tinymcp.py /dev/cu.usbmodem1101
```

The bridge keeps no protocol state.  When its stdin closes it sends
`notifications/cancelled` for every `subscriptions/listen` it relayed,
since the device has no connection to see close.

## Building and proving

```
make                 # examples/posix/tinymcp_posix
make test            # the protocol against a fake host, under the sanitizers
make mutants         # each check fails on a build that breaks what it guards
make conformance     # the official mcp SDK client in three modes, in .venv
make test-bridge     # the bridge against a fake device on a pty and a socket
make check           # all of the above
make size            # code size on a Cortex-M33 (arm-none-eabi-gcc)
make lint            # comment and prose style over the tracked files
```

`make conformance` creates `.venv` with the `mcp` and `pyserial` packages
on first use.  Each era of the conformance run lists, calls, reads, pages,
subscribes and sees a change arrive from the example server.

## Licence

Apache License 2.0, the licence of tikuOS; see `LICENSE`.
