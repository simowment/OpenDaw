# OpenDaw agent API (MCP)

OpenDaw can be driven by external AI agents (Claude Code, Codex, any MCP
client, or your own scripts). They use the same tools as the in-app AI chat:
tracks, clips, MIDI notes, effects, routing, mixing, patterns, and offline
rendering with audio analysis.

The API is the [Model Context Protocol](https://modelcontextprotocol.io)
(MCP, protocol version `2025-06-18`; clients asking for `2025-03-26` or
`2024-11-05` are accepted). It is served by the running OpenDaw and is only
reachable from the same user account on the same machine.

- Setting up Claude Code, Codex and other clients: [mcp-config-examples.md](mcp-config-examples.md)
- Python without MCP tooling: [`tools/python/opendaw_agent.py`](../tools/python/opendaw_agent.py)

## Architecture

```
 Claude Code / Codex / MCP client                    your script
        |  spawns, stdio                                  |
        v  (one JSON-RPC message per line)                |
 +---------------------+                                  |
 |  opendaw-mcp(.exe)  |  QtCore + QtNetwork only         |  opendaw_agent.py
 |  stdio <-> socket   |  (next to OpenDaw.exe)           |  (stdlib only)
 +---------------------+                                  |
        |  local socket: named pipe \\.\pipe\opendaw-agent-api (Windows)
        |                unix socket <tmp>/opendaw-agent-api (macOS, Linux)
        v  first message: opendaw/authenticate {token}    v
 +--------------------------------------------------------------------+
 | OpenDaw.exe (GUI thread = JUCE message thread)                       |
 |   LocalApiServer   QLocalServer, current user only, token check,    |
 |                    line framing, one message at a time              |
 |   CommandGateway   MCP methods: initialize, ping, tools/list,        |
 |                    tools/call  -> MCP results / JSON-RPC errors      |
 |   AiToolExecutor   schema validation, tool dispatch, run_batch,      |
 |                    built-in tools + extension tools (render,        |
 |                    patterns, ...)                                    |
 |   EditManager / te::Edit   the open project                         |
 +--------------------------------------------------------------------+
```

- Every tool call runs synchronously on OpenDaw's GUI thread, exactly like
  an edit made with the mouse; no other thread touches the project.
- Messages are handled one at a time, in arrival order, across all
  connections. A long call (a render) delays later messages until it
  finishes; their responses then follow in order.
- **Dialogs and menus pause the API.** While a modal dialog, a message box
  or a context menu is open (any nested event loop, or a modal window),
  requests wait in the queue and run as soon as it closes (OpenDaw retries
  every 50 ms). The code that opened the dialog may hold pointers to a track
  or a note that a tool call could delete. Calls are only delayed, never
  dropped. Clients with short per-call timeouts may see a timeout while the
  user leaves a dialog open.
- Authentication is not queued: the token message is answered at once, even
  during a long render or while a dialog is open.
- The agent's edits show up in the open windows immediately and are undoable
  (Ctrl+Z) like any other edit.
- `opendaw-mcp` holds no state of its own: it authenticates, then copies
  lines from stdin to the socket and back. It never writes anything but MCP
  messages to stdout; its log goes to stderr.
- Connecting never stalls the bridge. Each attempt (connect, check the
  server, authenticate) runs in the background and gives up after 5 s.
  Requests that arrive meanwhile are held, then forwarded, or answered
  offline if the attempt fails. With `--launch`, requests other than
  `initialize` wait up to 60 s for OpenDaw to start.

## Turning it on and off

The API starts with OpenDaw unless one of these disables it:

| Switch | Effect |
|---|---|
| `OPENDAW_AGENT_API=0` (environment) | off (`=1` forces it on) |
| `--no-agent-api` (OpenDaw command line) | off |
| setting `agent/apiEnabled` = false | off |
| setting `agent/allowDestructive` = false | destructive tools (and `run_batch` calls containing one) return `NOT_ALLOWED` |

## Security model

- **Local only.** There is no TCP port. The socket is created with
  `QLocalServer::UserAccessOption`: on Windows the pipe's ACL admits only the
  current user; on Unix the socket file is accessible to the owner only.
- **Token per run.** On start OpenDaw generates 32 random bytes
  (`QRandomGenerator::system()`), hex-encoded (64 characters), and writes them
  to the discovery file, which is readable by the owner only. A new token is
  made on every start.
- **Authenticate first.** The first message on every connection must be

  ```json
  {"jsonrpc":"2.0","id":0,"method":"opendaw/authenticate","params":{"token":"<64 hex chars>"}}
  ```

  which returns `{"ok":true,"serverName":"...","protocolVersion":"2025-06-18"}`.
  Any other first message, or a wrong token, gets JSON-RPC error `-32001`
  ("unauthenticated") and the connection is closed, and anything sent after
  it is dropped. A connection that does not authenticate within 15 s is
  closed. Tokens are compared in constant time.
- **Clients check the server before sending the token.** If OpenDaw crashes,
  its discovery file stays behind, and on Windows anyone on the machine can
  then create a pipe with the same name (the pipe namespace is shared by
  all users). A program doing that would receive the token and could serve
  its own tools to your agent. So `opendaw-mcp` and `opendaw_agent.py`
  check who serves the socket first:
  - Windows: `GetNamedPipeServerProcessId` must return the discovery file's
    `pid`, and that process must run as the current user (its token's user
    SID is compared with ours, which also covers a reused pid). The pid is
    compared only when the pipe name came from the discovery file (not with
    `--pipe` / `pipe=`).
  - POSIX: the peer's uid (`SO_PEERCRED` on Linux, `getpeereid` on macOS and
    BSD) must equal `getuid()`. A pid mismatch only logs a warning, because
    of pid namespaces and sandboxes.

  On failure nothing is sent. The bridge answers tool calls with
  "Refusing to send the token: ...". The Python client raises
  `OpenDawError("UNTRUSTED_SERVER")`; pass `verify_server=False` to skip
  the check. Custom clients should do the same check.
- **Limits.** One message is at most 16 MB; a longer line closes the
  connection.
- **What an agent can do.** Anything the tools allow on the open project,
  including deleting tracks and writing render files (in the render folder,
  or at an `output_path` it gives; existing files outside the render folder
  are never overwritten). Turn off `agent/allowDestructive` to block the
  destructive tools. MCP clients get `destructiveHint` / `readOnlyHint`
  annotations, so they can ask you before destructive calls.
- **Caveat.** Anyone running as your user can read the discovery file and use
  the API, as with any per-user local service.

## Discovery file

OpenDaw writes it on start and deletes it on exit. The bridge and the Python
client read it to find the socket and token. Location
(`<QStandardPaths::GenericConfigLocation>/OpenDaw/agent-api.json`, the same
for every program of the user, independent of application names):

| OS | Path |
|---|---|
| Windows | `%LOCALAPPDATA%\OpenDaw\agent-api.json` |
| macOS | `~/Library/Preferences/OpenDaw/agent-api.json` |
| Linux | `$XDG_CONFIG_HOME/OpenDaw/agent-api.json` (default `~/.config/...`) |

The environment variable `OPENDAW_AGENT_API_FILE` overrides the path for
OpenDaw, `opendaw-mcp` and `opendaw_agent.py` alike (useful for tests).

```json
{
    "name": "opendaw",
    "serverName": "opendaw-agent-api",
    "fullServerName": "\\\\.\\pipe\\opendaw-agent-api",
    "token": "3f9c...64 hex characters...",
    "requireToken": true,
    "pid": 12345,
    "protocolVersion": "2025-06-18",
    "appVersion": "0.4.0",
    "platform": "windows",
    "startedAt": "2026-11-21T17:58:48Z"
}
```

- `fullServerName` is the pipe path on Windows and the socket file path on
  Unix (`QDir::tempPath()` + name, usually `/tmp/opendaw-agent-api`; on macOS
  under `$TMPDIR`). Unix clients should connect to this absolute path.
- **Second instance.** If another live OpenDaw already owns
  `opendaw-agent-api`, the new instance listens on `opendaw-agent-api-<pid>`
  and takes over the discovery file. When it quits it puts the first
  instance's record back if that instance is still running.
- A stale socket file left by a crash is removed on the next start. A stale
  discovery file (after a crash) makes clients report "not reachable", or,
  if something else now serves that pipe name, "Refusing to send the
  token" (see the security model).

## Protocol

Newline-delimited JSON-RPC 2.0 over the socket (and over the bridge's
stdin/stdout), UTF-8, one JSON object or batch array per line. Blank lines
are ignored. This is MCP's stdio framing.

| Method | Result |
|---|---|
| `opendaw/authenticate` | `{ok:true, ...}` (socket only; the bridge does it for you) |
| `initialize` | `protocolVersion`, `capabilities:{tools:{listChanged:false}}` (the bridge reports `true`), `serverInfo:{name:"opendaw", title, version}`, `instructions` |
| `notifications/initialized`, other `notifications/*` | no response |
| `ping` | `{}` |
| `tools/list` | `{tools:[{name, description, inputSchema, annotations}]}`, everything on one page (a `cursor` is ignored, no `nextCursor`) |
| `tools/call` `{name, arguments}` | `{content:[{type:"text", text}], structuredContent, isError}` |
| `resources/list`, `resources/templates/list`, `prompts/list` | empty lists (not advertised, answered for lenient clients) |

The server is lenient: `tools/list` and `tools/call` work without
`initialize`.

**Tool annotations:** `readOnlyHint` (the tool does not change the project),
`destructiveHint` (deletes or replaces something, or undo/redo),
`idempotentHint:false`, `openWorldHint:false`.

**Tool results.** `structuredContent` is the tool's result object; `text` is
the same object as compact JSON. List results are wrapped as `{"items":[...]}`
and plain messages as `{"message":"..."}`. A failed call is still a normal
result, with `isError: true` and

```json
{"error": {"code": "NOT_FOUND", "message": "Track not found: track:99. Use get_track_list for ids.",
           "details": {"track": "track:99"}}}
```

Inputs are validated against `inputSchema` before a tool runs (types,
required, enum, minimum/maximum, items, oneOf; unknown properties are
rejected), so a typo fails fast with `INVALID_ARGUMENT` and changes nothing.

### Error codes in tool results

| Code | Meaning |
|---|---|
| `INVALID_ARGUMENT` | Input failed schema validation or a value is out of range |
| `NOT_FOUND` | A track, clip, plugin, pattern or file id/name is unknown |
| `BUSY` | A render/export or a recording is in progress; retry later |
| `UNKNOWN_TOOL` | No tool with that name (check `tools/list`) |
| `NOT_ALLOWED` | Not allowed here: inside `run_batch`, nested batch, or destructive tools disabled |
| `CONFLICT` | The current state does not allow it (e.g. no project loaded) |
| `BATCH_FAILED` | A command of an atomic `run_batch` failed; everything was rolled back |
| `FAILED` | The operation was attempted and failed |
| `INTERNAL` | Unexpected error (a tool threw); please report it |
| `UNAVAILABLE` | Bridge / Python client only: OpenDaw is not running or the connection dropped |
| `TIMEOUT` | Python client only: no answer within its timeout |

### JSON-RPC errors (malformed requests only)

| Code | Meaning |
|---|---|
| `-32700` | Parse error (the line is not JSON) |
| `-32600` | Invalid request (not JSON-RPC 2.0, missing method, empty batch, line > 16 MB) |
| `-32601` | Method not found |
| `-32602` | Invalid params (`params` not an object, `tools/call` without a string `name`, `arguments` not an object) |
| `-32603` | Internal error |
| `-32001` | Unauthenticated (connection is closed) |
| `-32000` | Bridge only: OpenDaw dropped or timed out a non-tool request |

## Ids

Every project item has a stable, typed id. Prefer ids over names and indices:
they survive renames, reordering, other tracks being deleted, and save/load
(they are Tracktion `EditItemID`s, stored in the `.tracktionedit` file).

| Id | Example | Where you get it |
|---|---|---|
| `track:<n>` | `track:1043` | `get_track_list`, `create_*_track` |
| `clip:<n>` | `clip:2210` | `get_clips_on_track`, `create_midi_clip` |
| `plugin:<n>` | `plugin:877` | `get_track_effects`, `get_track_info` |
| `pattern:<n>` | `pattern:3` | `list_patterns`, `create_pattern` |

Track parameters also accept `{"id":"track:1043"}`, a name (case-insensitive
exact match), a 0-based index, or `{"index":2,"name":"Drums"}`. A well-formed
id that matches nothing is `NOT_FOUND`; it never falls back to a name lookup.
Clip parameters take `clip_id` (preferred) or `clip_index`. Effect parameters
take a `plugin:` id, a name or an index.

Positions are in beats (quarter notes from the song start) unless the
parameter name says seconds or bars.

## Batches and rollback

`run_batch` runs several tool commands in one call and one undo step:

```json
{"name": "run_batch", "arguments": {
  "atomic": true,
  "commands": [
    {"tool": "set_track_volume", "input": {"track": "track:1043", "volume_db": -6}},
    {"tool": "set_track_pan",    "input": {"track": "track:1043", "pan": -0.2}},
    {"tool": "add_effect_to_track", "input": {"track": "track:1043", "effect_name": "compressor"}}
  ]}}
```

- Every command's input is validated before anything runs.
- `atomic` (default true): if a command fails, every change the batch made is
  undone and the call fails with `BATCH_FAILED`. `details` holds
  `failed_index`, `failed_tool`, `error {code, message}`, `rolled_back`,
  `undo_steps_reverted` and `results` (the commands before the failing one).
- `atomic: false`: commands that succeed stay. `stop_on_error` (default true)
  stops at the first failure; false runs every command and reports each.
- Success returns `{results:[{tool, ok, result|error}], count}`.
- Not allowed inside a batch (`NOT_ALLOWED`): transport (play/stop/record/
  set_position), rendering, save, undo/redo and `run_batch` itself.
- Rollback covers what is in the project's undo history (tracks, clips,
  notes, effects, routing, mix settings, patterns). It cannot undo things
  outside the project, such as files written to disk.
- The whole batch is one entry in OpenDaw's undo history ("Agent batch").
- Results of one command cannot be fed into a later command of the same
  batch; when you need a new id (e.g. from `create_pattern`), create the item
  first, then batch the rest.

## Listening: the render / analysis loop

Agents cannot hear, so OpenDaw renders offline and measures the audio:

1. Make a change (ideally in a `run_batch`).
2. `render_region {start_beat, end_beat, tracks?, tail_seconds?}` renders
   that range (only the given tracks, or the full mix) to a WAV file faster
   than real time and returns `{path, duration_seconds, ..., analysis}`.
   `analysis` has `integrated_lufs`, `peak_dbfs`, `true_peak_dbtp`,
   `rms_dbfs`, `crest_factor_db`, stereo correlation/width, a 7-band
   spectrum, onsets and plain-language `warnings`.
3. Adjust and render the same range again, then
   `compare_audio_files {path_a, path_b}` to see what changed (every delta is
   b minus a).

Also: `analyze_audio_file {path}` measures any audio file;
`export_stems {mode:"tracks"|"groups"}` renders stems. Renders block other
calls while they run and fail with `BUSY` while another render or a
recording is in progress. The bridge waits up to `--timeout` seconds
(default 600) for an answer.

## Examples

Raw session through the bridge (one line each; `>` is what you send):

```text
> {"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"demo","version":"1"}}}
< {"id":1,"jsonrpc":"2.0","result":{"capabilities":{"tools":{"listChanged":true}},"instructions":"OpenDaw is a digital audio workstation; ...","protocolVersion":"2025-06-18","serverInfo":{"name":"opendaw","title":"OpenDaw","version":"0.4.0"}}}
> {"jsonrpc":"2.0","method":"notifications/initialized"}
> {"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"create_midi_track","arguments":{"name":"Bass"}}}
< {"id":2,"jsonrpc":"2.0","result":{"content":[{"text":"{\"id\":\"track:1043\",\"index\":3,\"name\":\"Bass\"}","type":"text"}],"isError":false,"structuredContent":{"id":"track:1043","index":3,"name":"Bass"}}}
> {"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"set_track_volume","arguments":{"track":"track:9999","volume_db":-3}}}
< {"id":3,"jsonrpc":"2.0","result":{"content":[...],"isError":true,"structuredContent":{"error":{"code":"NOT_FOUND","message":"..."}}}}
```

A drum pattern, then listen to it:

```text
create_pattern     {"name":"Beat"}                                        -> {"id":"pattern:1", ...}
run_batch          {"commands":[
                     {"tool":"set_pattern_steps","input":{"pattern":"pattern:1","track":"track:1043","steps_string":"x...x...x...x..."}},
                     {"tool":"place_pattern","input":{"pattern":"pattern:1","start_bar":1}}]}
render_region      {"start_beat":0,"end_beat":4,"tracks":["track:1043"]}  -> {"path":"...wav","analysis":{...}}
```

Python (see the file's `__main__` for the full demo):

```python
from opendaw_agent import OpenDawClient, OpenDawError

with OpenDawClient() as daw:
    track = daw.call("create_midi_track", name="Lead")
    try:
        daw.batch([("set_track_volume", {"track": track["id"], "volume_db": -6}),
                   ("set_track_pan", {"track": track["id"], "pan": 0.3})])
    except OpenDawError as e:          # e.code == "BATCH_FAILED", e.details["failed_index"]
        print(e.code, e.message)
    print(daw.call("render_region", start_beat=0, end_beat=8)["analysis"]["integrated_lufs"])
```

## Troubleshooting

- **"OpenDaw is not running"** from the bridge: start OpenDaw, or add
  `--launch` to the bridge arguments. Clients that support
  `notifications/tools/list_changed` reload the tools on their own once OpenDaw
  is up; otherwise restart the MCP server in the client.
- **"unauthenticated: invalid token"**: the discovery file belongs to another
  run; restart OpenDaw (or check `OPENDAW_AGENT_API_FILE`).
- **"Refusing to send the token"** / `UNTRUSTED_SERVER`: the socket named in
  the discovery file is not served by that OpenDaw process (a stale file
  after a crash, or another program using the name). Start OpenDaw again.
- **Calls hang while a dialog is open**: expected. Requests wait until the
  dialog or menu closes (see "Dialogs and menus pause the API").
- **Bridge log**: `opendaw-mcp` logs to stderr; Claude Code shows it with
  `claude --debug` / `/mcp`.
- **Build lock on Windows**: a running `opendaw-mcp.exe` started by an MCP
  client locks the file; close the client (or the MCP server) before
  rebuilding.
