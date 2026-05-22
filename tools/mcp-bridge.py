#!/usr/bin/env python3
# Stdio→Unix-socket bridge for the Ants Terminal MCP server.
# ANTS-1255 — unblocks the entire MCP pack (ANTS-1244 / 1247 / 1248+).
#
# Reads line-delimited JSON-RPC from stdin, forwards each request to the
# Ants MCP socket at /tmp/ants-terminal-mcp-<PID>, writes the response
# back to stdout. One fresh socket connection per request (the Ants
# server is one-shot by design — see claudeintegration.cpp:1150-1397).
#
# Register once per machine:
#   claude mcp add ants -- /mnt/Games/Scripts/Linux/Ants_Terminal/tools/mcp-bridge.py
#
# Socket selection: $ANTS_MCP_SOCKET overrides; otherwise the newest
# /tmp/ants-terminal-mcp-* file by mtime wins. Multi-instance users
# wanting to pin a specific tab can `export ANTS_MCP_SOCKET=...`
# before launching Claude Code.

import glob
import json
import os
import socket
import sys

SOCK_GLOB = "/tmp/ants-terminal-mcp-*"
CONNECT_TIMEOUT_S = 2.0
READ_TIMEOUT_S = 5.0
MAX_REPLY_BYTES = 10 * 1024 * 1024  # match the server-side ceiling


def pick_socket() -> str:
    override = os.environ.get("ANTS_MCP_SOCKET")
    if override:
        return override
    # ANTS-1322: prefer sockets whose embedded PID is a live process
    # over stale leftovers from crashed instances. Pre-1322 the picker
    # used mtime alone and would happily return a socket whose
    # ants-terminal had crashed days ago, producing the "Failed to
    # connect" the user sees in `claude mcp list`.
    from stat import S_ISSOCK
    candidates = []
    for p in glob.glob(SOCK_GLOB):
        try:
            st = os.stat(p)
        except OSError:
            continue
        if not S_ISSOCK(st.st_mode):
            continue
        # Only connect to a socket we own. /tmp is world-writable + sticky,
        # so a co-tenant could pre-create an AF_UNIX listener at a live-PID
        # path and intercept our request stream. The C++ server enforces the
        # peer UID via SO_PEERCRED; the client must mirror it. indie-review-2026-05-21.
        if st.st_uid != os.getuid():
            continue
        # Path shape: /tmp/ants-terminal-mcp-<PID>
        pid_str = p.rsplit("-", 1)[-1]
        live = False
        try:
            pid = int(pid_str)
            os.kill(pid, 0)  # signal 0 = liveness probe, no signal sent
            live = True
        except (ValueError, ProcessLookupError, PermissionError):
            # ValueError: filename didn't carry a PID (custom socket)
            # ProcessLookupError: PID is gone — stale socket
            # PermissionError: PID exists but not ours (different user)
            #   — treat as not-live for our purposes.
            live = False
        # Live-PID sockets always rank above stale ones, mtime breaks
        # ties within each tier (a user with two live Ants instances
        # gets the most-recently-active one).
        candidates.append((1 if live else 0, st.st_mtime, p))
    if not candidates:
        return ""
    candidates.sort(reverse=True)
    return candidates[0][2]


def err_reply(req_id, code: int, message: str) -> dict:
    return {"jsonrpc": "2.0", "id": req_id,
            "error": {"code": code, "message": message}}


def forward(req: dict):
    is_notification = ("id" not in req) or (req.get("id") is None)
    req_id = req.get("id")

    sock_path = pick_socket()
    if not sock_path:
        return None if is_notification else err_reply(
            req_id, -32000, "no Ants Terminal MCP socket found")

    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(CONNECT_TIMEOUT_S)
        s.connect(sock_path)
        s.sendall(json.dumps(req).encode("utf-8") + b"\n")
        if is_notification:
            s.close()
            return None
        s.settimeout(READ_TIMEOUT_S)
        buf = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            if len(buf) > MAX_REPLY_BYTES:
                s.close()
                return err_reply(req_id, -32000,
                                 "reply exceeded 10 MiB ceiling")
        s.close()
        if not buf:
            return err_reply(req_id, -32000, "empty reply from Ants MCP")
        # ANTS-1769 — the server appends a '\n' end-of-reply terminator.
        # Its presence means the reply arrived complete; its absence on a
        # parse failure means the server closed mid-write (truncation),
        # which is a transport fault (-32000), NOT a malformed reply
        # (-32700). Back-compatible with an older server that sends no
        # terminator: a complete reply still parses, so we only fall into
        # the truncation branch when json.loads actually fails.
        had_terminator = buf.endswith(b"\n")
        payload = buf[:-1] if had_terminator else buf
        try:
            return json.loads(payload)
        except json.JSONDecodeError as e:
            if not had_terminator:
                return err_reply(
                    req_id, -32000,
                    f"truncated reply from Ants MCP (connection closed "
                    f"mid-write): {e}")
            return err_reply(req_id, -32700,
                             f"malformed reply from Ants MCP: {e}")
    except (OSError, socket.timeout) as e:
        return None if is_notification else err_reply(
            req_id, -32000, f"Ants MCP transport: {e}")


def main() -> int:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as e:
            sys.stdout.write(json.dumps(err_reply(
                None, -32700, f"client sent invalid JSON: {e}")) + "\n")
            sys.stdout.flush()
            continue
        reply = forward(req)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
