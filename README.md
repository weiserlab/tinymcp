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
tinymcp_http.h, tinymcp_http.c  Streamable HTTP for the 2026-07-28 revision
examples/posix/                 a server over stdio: a directory as resources and prompts, a shell tool
bridge/tinymcp.py               an MCP client's stdio to a device's serial port or TCP port
bridge/attend.py                answers a board's questions from the Anthropic API, opt-in
tests/                          the protocol against a fake host; mutants; the SDK; the bridge
ports/                          what each host maps
hygiene/                        the commit and style checks
```

| Measured on 2026-10-28 | |
|---|---|
| code, `-Os`, Cortex-M33 (`make size`) | 17.3 KB (`tinymcp.c` 14.4 KB, `tinymcp_json.c` 2.8 KB), and `tinymcp_http.c` 4.4 KB for HTTP |
| code, `-Os`, Cortex-M0+ | 17.5 KB, and 4.5 KB for HTTP |
| one HTTP connection's parser, `tinymcp_http_t` | 620 bytes |
| one instance, `tinymcp_t`, four subscription slots, two kept calls, Cortex-M33 | 900 bytes |
| stack during a tool call | about 1 KB (the argument table) |
| buffers | the host's: the in buffer holds one request, the out buffer one reply, or one piece of a longer one |

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

A tool whose work outlasts the request calls `tinymcp_defer()` inside
`tool_call` and returns `TINYMCP_LATER`.  The library keeps the call's id
and its `_meta.progressToken`, up to `TINYMCP_LATER_MAX` (2) calls at once,
and other requests are served meanwhile.  The host sends
`notifications/progress` with `tinymcp_progress()` and finally writes the
result between `tinymcp_result_begin()` and `tinymcp_result_end()`.  A
`notifications/cancelled` naming the call reaches the host's `cancel`, and
no result is sent for it; `tinymcp_closed()` cancels every kept call.
These calls are made from outside the host functions, from the host's
loop.

`tinymcp_meta()` reads a string member of the request's `params._meta`
while a host function serves it: the place a credential travels, such as
the key tikuOS checks on a TCP connection (`org.tinymcp/key`).

`name`, `title` and `version` are the host's `serverInfo`; the library
names itself in the `org.tinymcp/library` member of the `_meta` of the
discover and initialize results.  `tinymcp_client()` is the name the
client gave in its `clientInfo`, during a request: from `initialize` for
the handshake era, from each request's `_meta` for the current revision.

A tool writes its output through `tinymcp_sink_putc()`, so a shell whose
output function takes a character can be pointed at the reply directly.
Without `send_part` in the table, output past the out buffer is cut,
marked `[cut]`, and the result carries `isError`.  With it, a resource's
text, a tool's content and a kept call's result go out in pieces as they
are written, the out buffer holding one piece; a reply that fits still
goes whole through `send`.  Once a piece has gone the reply is committed:
a host that fails after it ends it short, a tool's with `isError`.

Before writing anything, a resource read or a tool can call
`tinymcp_sink_binary()` with a MIME type: the bytes that follow go out as
base64, a `blob` for a resource, `image` or `audio` content for a tool.
A tool with an `output_schema` returns its typed result through
`tinymcp_structured()`, which becomes `structuredContent` beside the
text.

## The protocol it serves

| Method | Era |
|---|---|
| `server/discover` | 2026-07-28 |
| `initialize`, `notifications/initialized`, `ping` | handshake eras |
| `tools/list`, `tools/call` | both |
| `resources/list`, `resources/templates/list`, `resources/read` | both |
| `prompts/list`, `prompts/get`, `completion/complete` | both |
| `subscriptions/listen` | 2026-07-28 |
| `notifications/progress`, `notifications/cancelled` | both |
| `resources/subscribe`, `resources/unsubscribe` | handshake eras |

A request that carries `_meta.io.modelcontextprotocol/protocolVersion` is
served statelessly as the 2026-07-28 revision asks, with
`clientCapabilities` required and an unsupported version answered by
`-32022`; `initialize` switches the stream to the handshake era for the
requests that follow.  `input_required` is served to a client of the
current revision; the handshake era's direct `elicitation/create` is not.
Text, blob, image and audio content and structured results; no
sampling.

## Serving HTTP

`tinymcp_http.c` serves the 2026-07-28 revision's Streamable HTTP over any
byte connection the host has: one `tinymcp_http_t` in front of one
`tinymcp_t` per connection, the host's TCP bytes fed in with
`tinymcp_http_feed()`, and the server's `send` and `send_part` passed to
`tinymcp_http_send()` and `tinymcp_http_send_part()`.  A request is a POST
to the host's path, its body read into the instance's in buffer.  The
`MCP-Protocol-Version`, `Mcp-Method` and `Mcp-Name` headers are checked
against the body (`-32020` on a mismatch or a repeat), `Accept` must carry
JSON and the event stream (406), `Content-Type` JSON (415), and an `Origin`
the host did not list is refused (403); another method is 405, another path
404, a body past the buffer 413, and these keep the connection by reading
past the body.  A notification is accepted (202) and dropped, as the
revision defines none over HTTP; `initialize` is refused for want of
`_meta`.  A reply that fits goes out as JSON with its length and the
status the spec gives its error code; one past the out buffer goes in
chunks; a notification before the reply, a kept call's progress or a
listen's updates, turns the response into an event stream, which the reply
ends.  A client cancels by closing the connection, and the host's
`tinymcp_closed()` then cancels the kept call.  `tinymcp_http_bearer()` is
the request's `Authorization: Bearer` token for the host to judge.

```
./build/tinymcp_posix --dir . --http 8080     # POST http://127.0.0.1:8080/mcp
```

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
since the device has no connection to see close.  With `TINYMCP_KEY` in its
environment it puts that key in every request's `_meta` as
`org.tinymcp/key`; the key never appears on its command line.

```
TINYMCP_KEY=... claude mcp add board -- python3 bridge/tinymcp.py --tcp 172.16.7.2:7000
```

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
make conformance     # the official mcp SDK client, three stdio modes and HTTP, in .venv
make test-bridge     # the bridge against a fake device on a pty and a socket, and its key
make test-attend     # attend.py against a stub of the Messages API
make check           # all of the above
make size            # code size on a Cortex-M33 (arm-none-eabi-gcc)
make lint            # comment and prose style over the tracked files
```

`make conformance` creates `.venv` with the `mcp` and `pyserial` packages
on first use.  Each era of the conformance run lists, calls, reads, pages,
subscribes and sees a change arrive from the example server, gets a
prompt, completes a file name, hears the prompts change, removes a file
only after its elicitation callback says yes, and follows the example's
`count` tool: its progress, an echo answered while it counts, a cancel;
it reads a 108 KB file and a 108 KB tool result whole through a 32 KB out
buffer, a binary file as a blob, an image from a tool and a structured
result checked against its output schema.  The same run goes over the
example's HTTP port through the SDK's Streamable HTTP client, beside the
wire rules it does not exercise.

## Licence

Apache License 2.0, the licence of tikuOS; see `LICENSE`.
