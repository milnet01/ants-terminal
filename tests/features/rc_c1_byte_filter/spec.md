# Feature: rc-socket C1 8-bit control-byte strip

Canonical design: `docs/specs/ANTS-1335.md`. This file is the
test-side restatement of the invariants the test_*.cpp pins.

## Problem

`RemoteControl::filterControlChars` (`remotecontrol.h:95`) strips C0
control bytes from `send-text` / `launch.command` / `new-tab.command`
payloads before they reach the PTY. C1 control codepoints
(U+0080..U+009F) — CSI / OSC / DCS / APC / PM / SOS in their 8-bit
form — are NOT stripped today. A same-UID rc/MCP peer can deliver
the UTF-8 encoding of U+009B (`0xC2 0x9B`, the 8-bit CSI introducer)
in any `text` or `command` field and reach the PTY unchanged.
`vtparser` (`vtparser.cpp:274`) routes 8-bit C1 introducers to the
same dispatcher as the 7-bit ESC-led twins, so the bytes drive the
parser into CSI / OSC 52 / DCS / APC states without ever needing an
`ESC` byte.

The C0 strip was hardened against this class of attack in 0.7.52;
this is the 8-bit counterpart.

## External anchors

- ECMA-48 § 5.3 — 7-bit and 8-bit code-extension. Tables for the
  C0 (0x00–0x1F) and C1 (0x80–0x9F) control sets.
- Williams VT500 state machine — CSI / OSC / DCS / APC entry from
  both 7-bit ESC + introducer and 8-bit introducer byte.
- ANTS-1294 / ANTS-1295 — same defense-in-depth philosophy applied
  to MCP response sanitisation and path-typed arguments.

## Invariants (full list in `docs/specs/ANTS-1335.md`)

- **INV-2** `0xC2 [0x80..0x9F]` strips atomically. The two bytes
  removed together, `out_stripped` incremented by 2.
- **INV-3** Bare `0xC2` at end-of-input is preserved (malformed
  UTF-8 in → malformed UTF-8 out).
- **INV-5** `0xC3 [0x80..0xBF]` and all other valid 2-byte UTF-8
  pass through unchanged.
- **INV-6** 3- and 4-byte UTF-8 sequences pass through unchanged
  (CJK, emoji).
- **INV-7** `out_stripped` counts bytes removed, not codepoints
  (a single C1 strip increments by 2).

## What the C++ test pins

PV-1..PV-16 exercise `filterControlChars` directly (no Qt event
loop). WI-1..WI-3 assert source-grep invariants:

- WI-1 — the new `b == 0xC2 &&` guarded block exists exactly once
  in `remotecontrol.h`.
- WI-2 — the obsolete "Stripping C1 is the AI-dialog layer's job"
  comment no longer appears.
- WI-3 — each of `cmdSendText`, `cmdLaunch`, `cmdNewTab` still
  calls `filterControlChars` (regression against accidental
  removal).

Together they pin the byte-level behaviour and the call-site wiring.
The end-to-end vtparser routing test (INT-1 in the canonical spec)
is left for a future iteration — `vtparser`'s C1 dispatch is already
covered by the in-tree vt-state-machine tests.
