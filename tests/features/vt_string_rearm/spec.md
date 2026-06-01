# Feature: VT parser re-arms string introducers after a string-terminating ESC

## Contract

The parser ends an OSC/DCS/APC/SOS-PM string when it sees a mid-stream
`ESC`, then enters a peek-state (`OscStringEsc` / `DcsStringEsc` /
`ApcStringEsc` / `IgnoreStringEsc`) that examines the next byte. The
0.7.53 hardening (`vt_osc_esc_discard`) made that peek-state discard the
byte unconditionally, so a hostile string ending in `ESC c` could not
trigger RIS — but back-to-back string sequences (`ESC P … ESC P …`,
`ESC _ … ESC _ …`, `ESC ] … ESC ] …`) lost their second introducer.

ANTS-1777 makes the peek-state **re-arm** — re-enter a string-collecting
state — *only* when the next byte is a string introducer
(`]`→OSC, `P`→DCS, `_`→APC, `X`→SOS, `^`→PM). Every other byte —
including the dangerous ESC-finals (`c`/RIS, `D`, `M`, `7`, `8`, `=`,
`>`), the CSI introducer `[`, intermediates, and C0 — is still
discarded. Design spec: `docs/specs/ANTS-1777.md`.

## Invariants

**INV-1 — Back-to-back DCS re-arms.** `ESC P AA ESC P BB ST` dispatches
two `DcsEnd` actions with payloads `AA` and `BB`; `BB` does not Print.

**INV-2 — Back-to-back APC re-arms.** `ESC _ AA ESC _ BB ST` dispatches
two `ApcEnd`, payloads intact.

**INV-3 — Back-to-back OSC re-arms.** `ESC ] o1 ESC ] o2 BEL` dispatches
two `OscEnd`, payloads `o1`/`o2` intact. (One case here uses the raw
8-bit ST `0x9C` to confirm the C1-ST close path.)

**INV-4 — Cross-type re-arm.** `ESC ] o1 ESC P d1 ST` → one `OscEnd`
then one `DcsEnd`.

**INV-5 — Security preserved (dangerous finals still discarded).** For
each of `c D M 7 8 = > (`, the stream `ESC P payload ESC <b>` yields
exactly one `DcsEnd` and zero `EscDispatch`, and the parser returns to
Ground (a trailing printable Prints).

**INV-6 — CSI stays discarded.** `ESC ] o1 ESC [ 2 J` yields one
`OscEnd`, zero `CsiDispatch` over the whole stream (checked after `J`),
and `2`/`J` reach Print (the `[` is discarded to Ground).

**INV-7 — SOS/PM re-arm is observable and swallows its payload.**
`ESC X aaa ESC X bbb ST Z` re-arms the second `X` into `IgnoreString`,
so `bbb` is swallowed (not Printed) and only the trailing `Z` Prints;
zero `EscDispatch`. (Pre-fix this discards the second `X` to Ground,
Printing `bbb`.)

## Scope

### In scope
- Re-arm of OSC/DCS/APC/SOS-PM introducers from every peek-state.
- The security boundary: non-introducer bytes stay discarded.

### Out of scope
- 8-bit C1 introducers after a 7-bit ESC terminator (mixed-encoding;
  see design spec § 5).
- Handler-level semantics (Sixel/Kitty rendering) — the parser change
  is transparent to handlers (design spec § 2.1).

## Test strategy

In-process scalar `VtParser` with a callback recording every `VtAction`.
Streams built by `+=` of separate `"\x1B"` / introducer pieces so no
`\x` escape gobbles a following hex digit. Print checks accept both the
scalar `codepoint` and the run `printRun`/`printRunLen` shapes. Verify
INV-1..7 FAIL against pre-fix source (where the second introducer is
discarded) before the `reArmStringIntroducer` helper lands.
