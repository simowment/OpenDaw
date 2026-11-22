# Connecting MCP clients to OpenDaw

Agents talk to OpenDaw through `opendaw-mcp`, a small MCP stdio server that
ships next to the OpenDaw executable. The client starts it; it connects to the
running OpenDaw (see [AGENT_API.md](AGENT_API.md) for how that works).

## Where is opendaw-mcp?

| Install | Path |
|---|---|
| Windows installer | `C:\Program Files\OpenDaw\opendaw-mcp.exe` (or wherever you installed OpenDaw) |
| Windows zip / dev build | next to `OpenDaw.exe`, e.g. `build\OpenDaw_artefacts\Debug\opendaw-mcp.exe` |
| macOS | `/Applications/OpenDaw.app/Contents/MacOS/opendaw-mcp` |
| Linux (built from source) | `build/OpenDaw_artefacts/Release/opendaw-mcp` |
| Linux AppImage | the AppImage cannot run it directly; extract it (`./OpenDaw.AppImage --appimage-extract`) and use `squashfs-root/usr/bin/opendaw-mcp` |

A dev build on Windows needs the Qt DLLs: run `windeployqt` on the folder, or
put `C:\Qt\6.10.2\msvc2022_64\bin` on the `PATH` the client passes to the
server (see the `env` examples below). Release builds already have them.

## Options

| Option | Meaning |
|---|---|
| `--launch` | Start OpenDaw (found next to `opendaw-mcp`, or `$OPENDAW_EXE`) when it is not running, and wait up to 60 s for it |
| `--timeout <s>` | How long to wait for OpenDaw to answer one request (default 600; renders can be long) |
| `--pipe <name>` | Connect to this server name / socket path instead of the one in the discovery file |
| `--discovery <path>` | Read the discovery file from here (also: `OPENDAW_AGENT_API_FILE`) |

Without `--launch`, start OpenDaw yourself. If it is not running, the bridge
still starts, lists no tools and answers every call with an error that says to
start OpenDaw; when OpenDaw comes up it sends `notifications/tools/list_changed`
so clients reload the tools.

## Claude Code

One command (user scope, so every project can use it):

```powershell
claude mcp add --scope user opendaw -- "C:\Program Files\OpenDaw\opendaw-mcp.exe" --launch
```

macOS / Linux:

```sh
claude mcp add --scope user opendaw -- /Applications/OpenDaw.app/Contents/MacOS/opendaw-mcp --launch
```

Or per project, in `.mcp.json` at the project root (check it in to share it):

```json
{
  "mcpServers": {
    "opendaw": {
      "command": "C:\\Program Files\\OpenDaw\\opendaw-mcp.exe",
      "args": ["--launch"]
    }
  }
}
```

A dev build with Qt from the Qt installer:

```json
{
  "mcpServers": {
    "opendaw": {
      "command": "C:\\Users\\me\\code\\OpenDaw\\build\\OpenDaw_artefacts\\Debug\\opendaw-mcp.exe",
      "args": ["--launch"],
      "env": { "PATH": "C:\\Qt\\6.10.2\\msvc2022_64\\bin;C:\\Windows\\System32" }
    }
  }
}
```

Check it with `claude mcp list` or `/mcp` inside Claude Code. Long renders:
Claude Code's tool-call timeout can be raised with the environment variable
`MCP_TOOL_TIMEOUT` (milliseconds), and the start-up timeout with
`MCP_TIMEOUT`. With `--launch` the first tool call may wait while OpenDaw
starts.

Try: *"Use OpenDaw: make a 4-bar drum pattern at 120 BPM, render it and tell me
how loud it is."*

## Codex

`~/.codex/config.toml` (Windows: `%USERPROFILE%\.codex\config.toml`):

```toml
[mcp_servers.opendaw]
command = 'C:\Program Files\OpenDaw\opendaw-mcp.exe'
args = ["--launch"]
# OpenDaw may need a few seconds to start, and renders can take a while.
startup_timeout_sec = 30
tool_timeout_sec = 600
```

macOS / Linux:

```toml
[mcp_servers.opendaw]
command = "/Applications/OpenDaw.app/Contents/MacOS/opendaw-mcp"
args = ["--launch"]
tool_timeout_sec = 600
```

Dev build on Windows (Qt DLLs from the Qt installer):

```toml
[mcp_servers.opendaw]
command = 'C:\Users\me\code\OpenDaw\build\OpenDaw_artefacts\Debug\opendaw-mcp.exe'
args = ["--launch"]
env = { PATH = 'C:\Qt\6.10.2\msvc2022_64\bin;C:\Windows\System32' }
```

Recent Codex CLIs can also add it from the command line:
`codex mcp add opendaw -- "C:\Program Files\OpenDaw\opendaw-mcp.exe" --launch`.

## Other MCP clients (Claude Desktop, Cursor, VS Code, ...)

Any client that runs stdio MCP servers works with the same three facts:

- **command:** the path to `opendaw-mcp`
- **args:** `["--launch"]` (optional)
- **transport:** stdio

Claude Desktop (`claude_desktop_config.json`), Cursor (`.cursor/mcp.json`) and
most others use the same shape as `.mcp.json`:

```json
{
  "mcpServers": {
    "opendaw": { "command": "/path/to/opendaw-mcp", "args": ["--launch"] }
  }
}
```

VS Code (`.vscode/mcp.json`):

```json
{
  "servers": {
    "opendaw": { "type": "stdio", "command": "/path/to/opendaw-mcp", "args": ["--launch"] }
  }
}
```

## Without an MCP client

- Python: `python tools/python/opendaw_agent.py` runs a demo; import
  `OpenDawClient` from it for your own scripts (stdlib only).
- Anything else: open the socket from the discovery file, send
  `opendaw/authenticate` with the token, then MCP JSON-RPC, one message per
  line ([AGENT_API.md](AGENT_API.md#protocol)).
- Quick manual test of the bridge (PowerShell):

  ```powershell
  '{"jsonrpc":"2.0","id":1,"method":"tools/list"}' | & "C:\Program Files\OpenDaw\opendaw-mcp.exe"
  ```
