# Feature: VT parser discards trailing byte on ESC inside string state

## Problem

Pre-0.7.53, the VT parser's `OscString` (and similarly `DcsString`,
`ApcString`, `IgnoreString`) handled an ESC byte mid-stream by:

1. Dispatching the OSC end (correct).
2. Transitioning to the `Escape` state (incorrect).

The Escape state's normal job is to interpret the next byte as the
selector of an ESC X sequence. So a crafted OSC body ending in `ESC c`
caused the parser to:

1. End the OSC.
2. Read the next byte (`c`).
3. Dispatch `c` as `EscDispatch` → `RIS` (full terminal reset).

Other RCE-adjacent fallouts: `ESC D` (IND → line feed),
`ESC M` (RI → reverse line feed), `ESC 7` / `ESC 8` (DECSC / DECRC),
`ESC =` / `ESC >` (DECPAM / DECPNM keypad mode), and the entire
charset-designator family.

The OSC body is attacker-supplied through:

- Trigger rules (regex output mapped through user-defined templates
  that may incorporate matched groups from PTY output).
- SSH (a remote shell can emit OSC bytes that reach the parser
  unfiltered).
- Pasted content at terminals where OSC dispatch is enabled.

So the consequence on a vulnerable terminal: a hostile remote shell
could `printf '\033]0;hello\033c'` and force a full terminal reset on
your local Ants instance, blowing away scrollback and tab state.
Or worse, chain it with cursor-positioning sequences to inject
output that looks like trusted local commands.

## External anchors

- [xterm ctlseqs §"OSC Ps ; Pt BEL"](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Operating-System-Commands)
  — documents that a string-state ESC ends the string regardless
  of what follows; the trailing byte is consumed.
- [xterm parser table — `vt500-state-charts.html`](https://invisible-island.net/xterm/ctlseqs/vt500-state-charts.html)
  — explicit state transitions for `OSC_STRING + ESC + ANY → ground`.
  No re-entry to Escape state, no dispatch of the trailing byte.
- [xterm CVE-2022-24130 (Mosh, but same parser shape)](https://nvd.nist.gov/vuln/detail/CVE-2022-24130)
  — closed the related class of "ESC inside DCS leaks dispatch" bugs.

## Contract

### Invariant 1 — `OscString + ESC + X` consumes both ESC and X

After feeding `ESC ] 0 ; t i t l e ESC c` to the parser:

- Exactly one `OscEnd` action is dispatched (with `oscString = "0;title"`).
- **Zero** `EscDispatch` actions are dispatched, regardless of what
  X is (`c` for RIS, `D` for IND, `7` for DECSC, etc.).
- The parser is back in Ground state.

The trailing byte is consumed silently.

### Invariant 2 — `OscString + ESC + \` is the same path

The legitimate 7-bit String Terminator `ESC \` is also handled by
the same OscStringEsc state. The OSC ends; the `\` is consumed; the
parser returns to Ground. (The visible `\` does NOT get printed.)

### Invariant 3 — DCS, APC, SOS/PM follow the same shape

`DcsString + ESC + X`, `ApcString + ESC + X`, `IgnoreString + ESC + X`
all consume X without dispatching it as an `EscDispatch`. Same
rationale: the ESC inside any string-state is the terminator for
that state, never the start of a fresh sequence.

### Invariant 4 — non-ESC string bytes are still accumulated

A regression that swung too far the other way (treating *any* byte
inside the string state as a terminator) would break legitimate OSC
8 / OSC 52 / OSC 1337 payloads. The spec requires that ordinary
bytes (non-BEL, non-ST, non-ESC) continue to flow into the OSC
buffer. Tested by feeding a 1 KiB OSC body and asserting the dispatched
oscString matches it byte-for-byte.

## Regression history

- **0.7.53** (2026-04-28) — root-cause fix per 2026-04-27 indie-review
  HIGH finding `vtparser.cpp:403`. Added `OscStringEsc` /
  `DcsStringEsc` / `ApcStringEsc` / `IgnoreStringEsc` peek-states to
  `enum State` and matching cases in `processChar`. Trailing byte
  dropped on the floor.

## Test strategy

In-process. Feed crafted byte streams into a `VtParser` whose
callback records every `VtAction` into a vector. Assert the recorded
sequence matches the expected dispatch shape for each invariant.
Tests run scalar parser only (no GUI dependencies, no `TerminalGrid`).
