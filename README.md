# tinymcp

tinymcp is the Model Context Protocol (MCP) server as a library that runs
on the device: C99, two files, no allocation, no operating system calls,
so a microcontroller's firmware can carry it and an agent can read, change
and watch the device through a cable.  It is not Golioth's tinymcp, which
runs on a host and reaches the device through their cloud.

A host binds the library by filling a table (its tools, its resources, its
prompts, a `send` function), owning two buffers, and feeding it bytes.  The library
answers every message of the 2026-07-28 revision of the protocol and of the
initialize-era revisions before it (2025-11-25 back to 2024-11-05), on the
same stream, and keeps the subscriptions a client opens.

tikuOS is the first host (`ports/tikuos.md`).  `examples/posix/` is a
complete host for a POSIX system and the conformance target.

## What it is

```
tinymcp.h, tinymcp.c            the server: eras, methods, errors, subscriptions, prompts
tinymcp_json.h, tinymcp_json.c  the scanner, in-place decoding, the bounded writer
examples/posix/                 a server over stdio: a directory as resources and prompts, a shell tool
bridge/tinymcp.py               an MCP client's stdio to a device's serial port or TCP port
bridge/attend.py                answers a board's questions from the Anthropic API, opt-in
tests/                          the protocol against a fake host; mutants; the SDK; the bridge
ports/                          what each host maps
hygiene/                        the commit and style checks
```

| Measured on 2026-10-27 | |
|---|---|
| code, `-Os`, Cortex-M33 (`make size`) | 14.8 KB (`tinymcp.c` 12.7 KB, `tinymcp_json.c` 2.1 KB) |
| code, `-Os`, Cortex-M0+ | 14.9 KB |
| one instance, `tinymcp_t`, four subscription slots, Cortex-M33 | 692 bytes |
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

Prompts come from `prompt_list` and `prompt_get`: the lister adds each
prompt and its arguments with `tinymcp_prompt_add()`, and the getter
writes the prompt's text, which goes out as one user message.
`tinymcp_placeholders()` names a template's `{{argument}}` places and
`tinymcp_render()` fills them, so a prompt can be a text file.  The host
calls `tinymcp_prompts_changed()` when the set of prompts changes.
`complete` offers values for a prompt's or a template's argument through
`tinymcp_complete_add()`; the reply keeps the first 100 that fit and
reports the rest in `total` and `hasMore`.

A host that needs the person's yes before it acts calls `tinymcp_elicit()`
with a form and a state and returns `TINYMCP_NEED_INPUT`; the library
answers with the 2026-07-28 revision's `input_required`, the client shows
the form and sends the request again, and the host reads the answer with
`tinymcp_input_action()`, `tinymcp_input_bool()` and
`tinymcp_request_state()`.  `tinymcp_client_elicits()` says whether the
request can carry that: a client of the current revision that declared
elicitation.

`name`, `title` and `version` are the host's `serverInfo`; the library
names itself in the `org.tinymcp/library` member of the `_meta` of the
discover and initialize results.  `tinymcp_client()` is the name the
client gave in its `clientInfo`, during a request: from `initialize` for
the handshake era, from each request's `_meta` for the current revision.

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
| `prompts/list`, `prompts/get`, `completion/complete` | both |
| `subscriptions/listen`, `notifications/cancelled` | 2026-07-28 |
| `resources/subscribe`, `resources/unsubscribe` | handshake eras |

A request that carries `_meta.io.modelcontextprotocol/protocolVersion` is
served statelessly as the 2026-07-28 revision asks, with
`clientCapabilities` required and an unsupported version answered by
`-32022`; `initialize` switches the stream to the handshake era for the
requests that follow.  `input_required` is served to a client of the
current revision; the handshake era's direct `elicitation/create` is not.
Text content only; no sampling, no HTTP.  A host
with an HTTP server wraps `tinymcp_message()`.

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

## Answering a board

A tikuOS board asks through `/sys/mcp/ask/question`, and any subscribed
client answers by writing `/sys/mcp/ask/answer`.  For a board left with
no agent attached, `bridge/attend.py` is that client: started by the owner
with `ANTHROPIC_API_KEY` in its environment, it sends each question to the
Messages API and writes the reply back, one line cut to fit.  The model is
told the board's instructions and the names of its prompts, nothing else;
`--max-tokens` (1000) bounds each reply and `--per-hour` (20) the
questions, past which the answer is a refusal; the model and the tokens of
every exchange go into the board's `/sys/mcp/journal`.  The bridge itself
never carries a key.

```
ANTHROPIC_API_KEY=... python3 bridge/attend.py /dev/cu.usbmodem1101
```

## Building and proving

```
make                 # examples/posix/tinymcp_posix
make test            # the protocol against a fake host, under the sanitizers
make mutants         # each check fails on a build that breaks what it guards
make conformance     # the official mcp SDK client in three modes, in .venv
make test-bridge     # the bridge against a fake device on a pty and a socket
make test-attend     # attend.py against a stub of the Messages API
make check           # all of the above
make size            # code size on a Cortex-M33 (arm-none-eabi-gcc)
make lint            # comment and prose style over the tracked files
```

`make conformance` creates `.venv` with the `mcp` and `pyserial` packages
on first use.  Each era of the conformance run lists, calls, reads, pages,
subscribes and sees a change arrive from the example server, gets a
prompt, completes a file name, hears the prompts change, and removes a
file only after its elicitation callback says yes.

## Licence

Apache License 2.0, the licence of tikuOS; see `LICENSE`.
