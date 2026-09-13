# Feature: a malformed CSI is swallowed, not printed

## Invariants

**INV-1 — DEL inside a CSI is ignored.** `ESC [ 1 DEL 2 m` dispatches one
`CsiDispatch` with final `m` and params `{12}`, and prints nothing.

**INV-2 — a private-marker byte after parameters discards the sequence.**
`ESC [ 1 ; < 5 m X` dispatches no CSI; the only printed text is `X`.

**INV-3 — a parameter byte after an intermediate discards the sequence.**
`ESC [ ! 1 p Y` dispatches no CSI; the only printed text is `Y`.

**INV-4 — a C0 control inside a discarded sequence still executes.**
`ESC [ 1 < LF m` emits an `Execute` for LF and dispatches no CSI.

**INV-5 — a leading private marker still dispatches.** `ESC [ ? 2 5 h`
dispatches final `h`, intermediate `?`, params `{25}`.

## Rationale

ECMA-48 and the DEC parser model (Paul Williams) ignore DEL in every CSI
state, and send a 0x3C–0x3F byte after parameters, or a 0x30–0x3F byte after
an intermediate, to a `csi_ignore` state that consumes up to the final byte
without dispatching. `VtParser` aborted to Ground on those bytes instead, so
the rest of the sequence printed as text.

## Test surface

`test_csi_ignore.cpp` feeds each byte string to a scalar `VtParser` and
inspects the actions its callback receives. No grid, no GUI.

## Regression history

- **ANTS-5075:** `CsiParam` and `CsiIntermediate` aborted to Ground on DEL
  and on out-of-place parameter bytes. Fixed with a `CsiIgnore` state. Locked
  by this spec.
