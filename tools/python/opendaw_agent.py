#!/usr/bin/env python3
"""Minimal Python client for the OpenDaw agent API (stdlib only).

OpenDaw listens on a local socket while it runs (a named pipe on Windows,
a Unix domain socket elsewhere) and speaks MCP: newline-delimited JSON-RPC
2.0. This module connects to it directly (no opendaw-mcp bridge needed),
authenticates with the per-run token from the discovery file and exposes the
tools as plain Python calls.

    from opendaw_agent import OpenDawClient, OpenDawError

    with OpenDawClient() as daw:
        print([t["name"] for t in daw.list_tools()])
        daw.call("set_tempo", bpm=124)
        daw.batch([
            ("create_midi_track", {"name": "Bass"}),
            ("set_time_signature", {"numerator": 4, "denominator": 4}),
        ], atomic=True)

Where things live
-----------------
Discovery file (written by OpenDaw on start, removed on exit), unless the
environment variable OPENDAW_AGENT_API_FILE points elsewhere:
    Windows: %LOCALAPPDATA%\\OpenDaw\\agent-api.json
    macOS:   ~/Library/Preferences/OpenDaw/agent-api.json
    Linux:   $XDG_CONFIG_HOME/OpenDaw/agent-api.json (default ~/.config)
It holds serverName, fullServerName, token, pid, protocolVersion, appVersion.

Socket:
    Windows: \\\\.\\pipe\\<serverName>     (default \\\\.\\pipe\\opendaw-agent-api)
    POSIX:   fullServerName, i.e. <QDir::tempPath()>/<serverName>
             (usually /tmp/opendaw-agent-api; $TMPDIR on macOS)

Before sending the token the client checks who serves the socket (pass
verify_server=False to skip): on Windows the pipe's server process must be
the discovery file's pid and run as the current user, because a pipe name
freed by a crash can be re-created by any account; on POSIX the peer uid
must be ours.

Run this file to see a demo that creates a MIDI track and a pattern, renders
and prints the analysis:  python opendaw_agent.py [--pipe NAME] [--discovery PATH]
"""

from __future__ import annotations

import itertools
import json
import os
import socket
import struct
import sys
import time
from typing import Any, Dict, Iterable, List, Optional, Tuple, Union

__all__ = ["OpenDawClient", "OpenDawError", "default_discovery_path", "read_discovery"]

PROTOCOL_VERSION = "2025-06-18"
DEFAULT_SERVER_NAME = "opendaw-agent-api"
MAX_LINE = 16 * 1024 * 1024


class OpenDawError(Exception):
    """A failed tool call (code is an AiErrorCode such as NOT_FOUND) or a
    JSON-RPC protocol error (code is the numeric JSON-RPC code as a string)."""

    def __init__(self, code: str, message: str, details: Any = None):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.details = details


def default_discovery_path() -> str:
    override = os.environ.get("OPENDAW_AGENT_API_FILE")
    if override:
        return override
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    elif sys.platform == "darwin":
        base = os.path.join(os.path.expanduser("~"), "Library", "Preferences")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "OpenDaw", "agent-api.json")


def read_discovery(path: Optional[str] = None) -> Dict[str, Any]:
    path = path or default_discovery_path()
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        raise OpenDawError("UNAVAILABLE", f"OpenDaw is not running (no discovery file at {path}).") from None


# ── Server identity ─────────────────────────────────────────────────────────

def _untrusted(why: str) -> OpenDawError:
    return OpenDawError("UNTRUSTED_SERVER",
                        f"Refusing to send the token: {why} The discovery file is probably stale "
                        "(did OpenDaw crash?). Start OpenDaw again.")


def _verify_pipe_server(handle: int, expected_pid: Optional[int]) -> None:
    """Windows: the pipe's server process is expected_pid and runs as us."""
    import ctypes
    from ctypes import wintypes

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    k32.GetNamedPipeServerProcessId.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.ULONG)]
    k32.GetNamedPipeServerProcessId.restype = wintypes.BOOL
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.GetCurrentProcess.argtypes = []
    k32.GetCurrentProcess.restype = wintypes.HANDLE
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    k32.CloseHandle.restype = wintypes.BOOL
    adv.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    adv.OpenProcessToken.restype = wintypes.BOOL
    adv.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                        ctypes.POINTER(wintypes.DWORD)]
    adv.GetTokenInformation.restype = wintypes.BOOL
    adv.EqualSid.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    adv.EqualSid.restype = wintypes.BOOL

    TOKEN_QUERY = 0x0008
    TOKEN_USER_CLASS = 1
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

    pid = wintypes.ULONG(0)
    if not k32.GetNamedPipeServerProcessId(wintypes.HANDLE(handle), ctypes.byref(pid)):
        raise _untrusted(f"cannot tell which process serves the pipe (error {ctypes.get_last_error()}).")
    if expected_pid and pid.value != expected_pid:
        raise _untrusted(f"the pipe is served by process {pid.value}, but the discovery file names "
                         f"OpenDaw pid {expected_pid}.")

    def token_user(process) -> Optional[ctypes.Array]:
        token = wintypes.HANDLE()
        if not adv.OpenProcessToken(process, TOKEN_QUERY, ctypes.byref(token)):
            return None
        try:
            size = wintypes.DWORD(0)
            adv.GetTokenInformation(token, TOKEN_USER_CLASS, None, 0, ctypes.byref(size))
            if not size.value:
                return None
            buf = ctypes.create_string_buffer(size.value)
            if not adv.GetTokenInformation(token, TOKEN_USER_CLASS, buf, size, ctypes.byref(size)):
                return None
            return buf
        finally:
            k32.CloseHandle(token)

    proc = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
    if not proc:
        raise _untrusted(f"cannot open the pipe's server process {pid.value} (error {ctypes.get_last_error()}).")
    try:
        theirs = token_user(proc)
    finally:
        k32.CloseHandle(proc)
    ours = token_user(k32.GetCurrentProcess())
    if theirs is None or ours is None:
        raise _untrusted(f"cannot read the user of process {pid.value} (error {ctypes.get_last_error()}).")
    # TOKEN_USER starts with SID_AND_ATTRIBUTES, whose first member is the PSID.
    their_sid = ctypes.cast(theirs, ctypes.POINTER(ctypes.c_void_p))[0]
    our_sid = ctypes.cast(ours, ctypes.POINTER(ctypes.c_void_p))[0]
    if not adv.EqualSid(their_sid, our_sid):
        raise _untrusted(f"the pipe's server process {pid.value} belongs to another user.")


def _verify_unix_peer(sock: socket.socket, expected_pid: Optional[int]) -> None:
    """POSIX: the socket's peer runs as our uid (the pid is only compared for a warning)."""
    peer_pid = 0
    if hasattr(socket, "SO_PEERCRED"):  # Linux
        creds = sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
        peer_pid, peer_uid, _gid = struct.unpack("3i", creds)
    else:  # macOS / BSD
        import ctypes
        import ctypes.util

        libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
        uid = ctypes.c_uint32(0)
        gid = ctypes.c_uint32(0)
        if libc.getpeereid(sock.fileno(), ctypes.byref(uid), ctypes.byref(gid)) != 0:
            raise _untrusted(f"cannot read the socket peer's credentials (errno {ctypes.get_errno()}).")
        peer_uid = uid.value
        if sys.platform == "darwin":
            try:
                peer_pid = struct.unpack("i", sock.getsockopt(0, 0x002, 4))[0]  # SOL_LOCAL, LOCAL_PEERPID
            except OSError:
                peer_pid = 0
    if peer_uid != os.getuid():
        raise _untrusted(f"the socket is served by another user (uid {peer_uid}).")
    if expected_pid and peer_pid and peer_pid != expected_pid:
        print(f"opendaw_agent: the socket is served by pid {peer_pid}, the discovery file names "
              f"pid {expected_pid}", file=sys.stderr)


# ── Transports ──────────────────────────────────────────────────────────────

class _PipeTransport:
    """Windows named pipe opened as a file (byte mode, unbuffered)."""

    def __init__(self, path: str, connect_timeout: float, verify: bool = True,
                 expected_pid: Optional[int] = None):
        deadline = time.monotonic() + connect_timeout
        while True:
            try:
                self._f = open(path, "r+b", buffering=0)
                break
            except FileNotFoundError:
                raise OpenDawError("UNAVAILABLE", f"No OpenDaw pipe at {path}.") from None
            except OSError as e:
                # ERROR_PIPE_BUSY (231): every instance is taken; try again shortly.
                if getattr(e, "winerror", None) == 231 and time.monotonic() < deadline:
                    time.sleep(0.05)
                    continue
                raise OpenDawError("UNAVAILABLE", f"Cannot open {path}: {e}") from None
        if verify:
            import msvcrt
            try:
                _verify_pipe_server(msvcrt.get_osfhandle(self._f.fileno()), expected_pid)
            except BaseException:
                self.close()
                raise

    def send(self, data: bytes) -> None:
        view = memoryview(data)
        try:
            while view:
                n = self._f.write(view)
                view = view[n or 0:]
        except OSError as e:
            raise OpenDawError("UNAVAILABLE", f"OpenDaw closed the connection ({e}).") from None

    def recv(self) -> bytes:
        try:
            return self._f.read(65536) or b""
        except OSError:
            return b""  # the server closed the pipe (broken pipe / invalid argument)

    def close(self) -> None:
        try:
            self._f.close()
        except OSError:
            pass


class _UnixTransport:
    def __init__(self, path: str, connect_timeout: float, timeout: Optional[float], verify: bool = True,
                 expected_pid: Optional[int] = None):
        self._s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._s.settimeout(connect_timeout)
        try:
            self._s.connect(path)
        except OSError as e:
            self._s.close()
            raise OpenDawError("UNAVAILABLE", f"Cannot connect to {path}: {e}") from None
        if verify:
            try:
                _verify_unix_peer(self._s, expected_pid)
            except BaseException:
                self.close()
                raise
        self._s.settimeout(timeout)

    def send(self, data: bytes) -> None:
        try:
            self._s.sendall(data)
        except (BrokenPipeError, ConnectionResetError) as e:
            raise OpenDawError("UNAVAILABLE", f"OpenDaw closed the connection ({e}).") from None

    def recv(self) -> bytes:
        try:
            return self._s.recv(65536)
        except ConnectionResetError:
            return b""

    def close(self) -> None:
        try:
            self._s.close()
        except OSError:
            pass


# ── Client ──────────────────────────────────────────────────────────────────

Command = Union[Tuple[str, Dict[str, Any]], Dict[str, Any]]


class OpenDawClient:
    """Synchronous client. One request at a time; not thread-safe.

    timeout applies to each response on POSIX (renders can take a while, so
    the default is generous). Windows pipes opened as files cannot time out.

    verify_server (default True) checks, before the token is sent, that the
    socket is served by OpenDaw: on Windows by the discovery file's pid
    (when the pipe name came from that file) running as the current user; on
    POSIX by a process of our uid. It raises OpenDawError("UNTRUSTED_SERVER").
    """

    def __init__(self, pipe: Optional[str] = None, token: Optional[str] = None,
                 discovery: Optional[str] = None, timeout: Optional[float] = 600.0,
                 connect_timeout: float = 5.0, initialize: bool = True, verify_server: bool = True):
        self._pipe = pipe
        self._verify_server = verify_server
        self._token = token
        self._discovery = discovery
        self._timeout = timeout
        self._connect_timeout = connect_timeout
        self._ids = itertools.count(1)
        self._buf = b""
        self._transport = None
        self.server_info: Dict[str, Any] = {}
        self.instructions: str = ""
        self.notifications: List[Dict[str, Any]] = []
        self._tools: Optional[List[Dict[str, Any]]] = None
        self._connect()
        if initialize:
            self.initialize()

    # context manager
    def __enter__(self) -> "OpenDawClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        if self._transport:
            self._transport.close()
            self._transport = None

    # ── connection ──────────────────────────────────────────────────────────

    def _connect(self) -> None:
        info: Dict[str, Any] = {}
        if self._token is None or self._pipe is None:
            try:
                info = read_discovery(self._discovery)
            except OpenDawError:
                if self._pipe is None:
                    raise
        token = self._token if self._token is not None else info.get("token", "")
        name = self._pipe or (info.get("serverName") if sys.platform == "win32" else
                              info.get("fullServerName") or info.get("serverName")) or DEFAULT_SERVER_NAME

        # Only an endpoint taken from the discovery file is bound to its pid.
        expected_pid: Optional[int] = None
        if not self._pipe:
            try:
                expected_pid = int(info.get("pid") or 0) or None
            except (TypeError, ValueError):
                expected_pid = None

        if sys.platform == "win32":
            path = name if name.startswith("\\\\") else "\\\\.\\pipe\\" + name
            self._transport = _PipeTransport(path, self._connect_timeout, self._verify_server, expected_pid)
        else:
            path = name if name.startswith("/") else os.path.join(
                os.environ.get("TMPDIR", "/tmp").rstrip("/") or "/tmp", name)
            self._transport = _UnixTransport(path, self._connect_timeout, self._timeout,
                                             self._verify_server, expected_pid)

        reply = self._roundtrip({"jsonrpc": "2.0", "id": 0, "method": "opendaw/authenticate",
                                 "params": {"token": token}})
        if "error" in reply or not reply.get("result", {}).get("ok"):
            err = reply.get("error", {})
            self.close()
            raise OpenDawError(str(err.get("code", -32001)), err.get("message", "authentication failed"))

    def initialize(self) -> Dict[str, Any]:
        result = self.request("initialize", {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {},
            "clientInfo": {"name": "opendaw_agent.py", "version": "1.0"},
        })
        self.server_info = result.get("serverInfo", {})
        self.instructions = result.get("instructions", "")
        self.notify("notifications/initialized")
        return result

    # ── JSON-RPC plumbing ───────────────────────────────────────────────────

    def _write(self, msg: Any) -> None:
        if not self._transport:
            raise OpenDawError("UNAVAILABLE", "The client is closed.")
        data = json.dumps(msg, separators=(",", ":"), ensure_ascii=False).encode("utf-8") + b"\n"
        self._transport.send(data)

    def _read_message(self) -> Any:
        while b"\n" not in self._buf:
            if len(self._buf) > MAX_LINE:
                raise OpenDawError("-32600", "Response line exceeds 16 MB.")
            try:
                chunk = self._transport.recv()
            except socket.timeout:
                raise OpenDawError("TIMEOUT", "OpenDaw did not answer in time.") from None
            if not chunk:
                self.close()
                raise OpenDawError("UNAVAILABLE", "OpenDaw closed the connection.")
            self._buf += chunk
        line, _, self._buf = self._buf.partition(b"\n")
        line = line.strip()
        if not line:
            return self._read_message()
        return json.loads(line.decode("utf-8"))

    def _roundtrip(self, msg: Dict[str, Any]) -> Dict[str, Any]:
        self._write(msg)
        want = msg["id"]
        while True:
            reply = self._read_message()
            if isinstance(reply, dict) and "method" in reply and "id" not in reply:
                self.notifications.append(reply)
                if reply.get("method") == "notifications/tools/list_changed":
                    self._tools = None
                continue
            if isinstance(reply, dict) and reply.get("id") == want:
                return reply
            # A response to something else (should not happen on a private connection).

    def request(self, method: str, params: Optional[Dict[str, Any]] = None) -> Any:
        msg: Dict[str, Any] = {"jsonrpc": "2.0", "id": next(self._ids), "method": method}
        if params is not None:
            msg["params"] = params
        reply = self._roundtrip(msg)
        if "error" in reply:
            err = reply["error"]
            raise OpenDawError(str(err.get("code")), err.get("message", ""), err.get("data"))
        return reply.get("result")

    def notify(self, method: str, params: Optional[Dict[str, Any]] = None) -> None:
        msg: Dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)

    def ping(self) -> None:
        self.request("ping")

    # ── Tools ───────────────────────────────────────────────────────────────

    def list_tools(self, refresh: bool = False) -> List[Dict[str, Any]]:
        """MCP tool definitions: name, description, inputSchema, annotations."""
        if self._tools is None or refresh:
            self._tools = self.request("tools/list", {}).get("tools", [])
        return self._tools

    def tool(self, name: str) -> Optional[Dict[str, Any]]:
        return next((t for t in self.list_tools() if t.get("name") == name), None)

    def has_tool(self, name: str) -> bool:
        return self.tool(name) is not None

    def call_raw(self, tool: str, arguments: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        """The MCP CallToolResult ({content, structuredContent, isError}); never raises for tool errors."""
        return self.request("tools/call", {"name": tool, "arguments": arguments or {}})

    def call(self, tool: str, arguments: Optional[Dict[str, Any]] = None, /, **kwargs: Any) -> Any:
        """Calls a tool and returns its structured result.

        Raises OpenDawError(code, message, details) when the tool reports an
        error. Arguments may be a dict, keywords, or both (keywords win).
        """
        args = dict(arguments or {})
        args.update(kwargs)
        result = self.call_raw(tool, args)
        structured = result.get("structuredContent")
        text = "".join(c.get("text", "") for c in result.get("content", []) if c.get("type") == "text")
        if result.get("isError"):
            err = (structured or {}).get("error", {}) if isinstance(structured, dict) else {}
            raise OpenDawError(err.get("code", "FAILED"), err.get("message", text), err.get("details"))
        if structured is not None:
            return structured
        try:
            return json.loads(text)
        except ValueError:
            return text

    def batch(self, commands: Iterable[Command], atomic: bool = True,
              stop_on_error: Optional[bool] = None) -> Any:
        """Runs several tool commands with run_batch, as one undo step.

        commands: (tool, input) tuples or {"tool": ..., "input": {...}} dicts.
        atomic=True rolls every change back if one command fails, and the call
        raises OpenDawError("BATCH_FAILED", ...) whose details say which step.
        """
        cmds = []
        for c in commands:
            if isinstance(c, dict):
                cmds.append({"tool": c["tool"], "input": dict(c.get("input", {}))})
            else:
                tool, inp = c
                cmds.append({"tool": tool, "input": dict(inp or {})})
        args: Dict[str, Any] = {"commands": cmds, "atomic": atomic}
        if stop_on_error is not None:
            args["stop_on_error"] = stop_on_error
        return self.call("run_batch", args)


# ── Demo ────────────────────────────────────────────────────────────────────

def _show(title: str, value: Any) -> None:
    print(f"\n== {title}")
    print(value if isinstance(value, str) else json.dumps(value, indent=2, ensure_ascii=False))


def _demo(argv: List[str]) -> int:
    import argparse

    ap = argparse.ArgumentParser(description="OpenDaw agent API demo: track + pattern + render + analysis")
    ap.add_argument("--pipe", help="server name or socket path (default: from the discovery file)")
    ap.add_argument("--discovery", help="discovery file path")
    ap.add_argument("--token", help="token (default: from the discovery file)")
    ns = ap.parse_args(argv)

    try:
        daw = OpenDawClient(pipe=ns.pipe, token=ns.token, discovery=ns.discovery)
    except OpenDawError as e:
        print(f"Could not connect: {e}", file=sys.stderr)
        return 1

    with daw:
        print(f"Connected to {daw.server_info.get('name')} {daw.server_info.get('version')}, "
              f"{len(daw.list_tools())} tools")
        try:
            # 1. A MIDI track (it gets the built-in instrument). Keep its id:
            #    ids stay valid across renames and reordering.
            track = daw.call("create_midi_track", name="Agent Demo")
            track_id = track["id"]
            print(f"Track {track['name']!r} -> {track_id}")

            # 2. A one-bar pattern; its id is needed by the next steps.
            pattern = daw.call("create_pattern", name="Agent Beat")
            pattern_id = pattern["id"]
            print(f"Pattern {pattern['name']!r} -> {pattern_id}")

            # 3. Steps and placement in one atomic batch (one undo step; if any
            #    command fails, everything is rolled back and BATCH_FAILED is raised).
            batch = daw.batch([
                ("set_pattern_steps", {"pattern": pattern_id, "track": track_id,
                                       "steps_string": "x...x...x...x.x."}),
                ("place_pattern", {"pattern": pattern_id, "start_bar": 1}),
            ], atomic=True)
            print(f"Batch ran {batch.get('count')} commands")

            # 4. Listen: render bar 1 of that track offline and read the analysis.
            render = daw.call("render_region", start_beat=0, end_beat=4, tracks=[track_id], tail_seconds=0.5)
        except OpenDawError as e:
            print(f"Failed: {e}", file=sys.stderr)
            if e.details:
                _show("details", e.details)
            return 1

        print(f"Rendered {render.get('path')} ({render.get('duration_seconds')} s)")
        analysis = render.get("analysis", {})
        keys = ("integrated_lufs", "peak_dbfs", "true_peak_dbtp", "rms_dbfs", "crest_factor_db")
        _show("analysis summary", {k: analysis.get(k) for k in keys if k in analysis})
        if analysis.get("onsets"):
            _show("onsets", analysis["onsets"])
        if analysis.get("warnings"):
            _show("warnings", analysis["warnings"])
    return 0


if __name__ == "__main__":
    sys.exit(_demo(sys.argv[1:]))
