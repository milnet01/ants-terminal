# Feature: rc get-text server-side byte cap

Canonical design: `docs/specs/ANTS-1348.md`. This file is the
test-side restatement of the invariants the test_*.cpp pins.

## Problem

`cmdGetText` (`remotecontrol.cpp:375-422`) caps the response at
10 000 lines but applies no byte cap. A scrollback of 10 000 × 4 KB
lines yields a 40 MB JSON envelope; the MCP-bridge 1 MiB receive
cap then truncates mid-stream and surfaces to the Claude Code
caller as a misleading "socket hijack" error. Fix: clamp on the
server at 1 MiB by default (matching the client cap), trim from
the *head* of the scrollback (keep newest lines), snap to a `\n`
boundary, prepend a `<truncated N bytes / M lines>\n` sentinel,
and emit `truncated` / `bytes_dropped` / `lines_dropped` in the
response envelope so the caller knows data was elided.

## External anchors

- The MCP bridge's 1 MiB receive cap lives in
  `claudeintegration.cpp` — every MCP response above that fails
  to parse on the assistant side.
- The 16 MiB hard ceiling is a rc-consumer rule of thumb (16× the
  MCP cap); audit pipeline's `MAX_TOOL_OUTPUT_BYTES` at
  `auditdialog.h:468` is a sibling 64 MiB "biggest tool output"
  constant — different consumer, different number.

## Invariants (full list in `docs/specs/ANTS-1348.md`)

- **INV-1** `out["bytes"]` ≤ `max_bytes` (or 16 MiB ceiling).
- **INV-2** Trim drops oldest; newest tail always survives.
- **INV-3** Truncation snaps to `\n` boundary when one exists.
- **INV-4** `out["truncated"]` always present (`false` on happy
  path, `true` otherwise).
- **INV-5** `bytes_dropped` / `lines_dropped` only on truncation.
- **INV-6** Truncated text starts with `<truncated N bytes / M lines>\n`.
- **INV-7** Default cap is 1 MiB.
- **INV-8** Ceiling is 16 MiB; over-ceiling silently clamped and
  flagged via `bytes_cap_clamped:true`.

## What the C++ test pins

GT-1..GT-10 exercise the pure trim helper directly (no Qt event
loop, no MainWindow). WI-1..WI-3 assert source-grep wiring on
`cmdGetText`:

- WI-1 — `cmdGetText` body references the literal `"max_bytes"`
  exactly once.
- WI-2 — the helper's default and ceiling constants are present
  in the file.
- WI-3 — `cmdGetText` body sets `out["truncated"]` so every
  code path emits the field.
