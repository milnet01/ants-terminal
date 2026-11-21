# ANTS-1777 — Safe re-arm of string introducers after a string-terminating `ESC` — review loop log

Record for [`../specs/ANTS-1777-safe-re-arm-string-introducers.md`](../specs/ANTS-1777-safe-re-arm-string-introducers.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-06-01)** — two independent cold reviewers (general +
security lens). Zero CRITICAL; core design (allowlist exactly matches
the Escape-state introducer set, excludes `[` and all finals; INV-5/6/7
hold) confirmed sound by both. Verified + folded:

- **HIGH (both)** — § 1 cited the peek-state *case labels*
  (`:518/:552/:588/:602`) where it meant the *transition sites*. Fixed
  to `:511/:545/:581/:599`; discard line for `IgnoreStringEsc` now cited
  explicitly (`:604`).
- **HIGH (security)** — "grants no capability beyond Ground" was
  asserted but not tied to the handler. Added the § 2.1 capability-parity
  paragraph (re-armed `OscEnd`/`DcsEnd`/`ApcEnd` dispatch identically via
  `handleOsc` `terminalgrid.cpp:1016`; chaining ≡ `BEL`/`ST`-separated
  sequences already reachable today; handlers never observe Ground).
- **MEDIUM** — INV-5 cross-ref overstated `vt_osc_esc_discard` INV-3
  coverage (it tests `c` only, not the full final set). Corrected.
- **MEDIUM** — INV-6 test window tightened: assert zero `CsiDispatch`
  *after* the final `J`, and that `2`/`J` reach `Print`, so a future
  `[`-allowlisting regression is actually caught.
- **MEDIUM** — `INV-N` label collision across the three docs noted in
  § 6 (ids are spec-local).
- **LOW** — added `(` (charset-designator intermediate) to INV-5's
  discarded set; added the per-segment-cap / no-new-DoS note to § 4;
  added the `**Layman:**` line; wrote this log.

**Loop 2 (2026-06-01)** — same two cold reviewers, re-read with no
briefing on loop-1 fixes. Loop-1 fixes all held (none resurfaced); all
line citations re-confirmed correct. New findings verified + folded:

- **HIGH (security)** — § 2.1's "no image-merge semantics ride on that
  visit" was factually wrong: `handleApc` *does* carry a Kitty chunk
  accumulator (`m_kittyChunkBuffer`/`m_kittyChunkId`,
  `terminalgrid.cpp:3244-3277`). Re-grounded the parity argument on the fact
  that the merge keys on the `m=`/`i=` APC params, **not** on a Ground
  visit, so re-arm is still transparent. Resolves the loop-1 open
  code-side question (answer: an accumulator exists, but re-arm doesn't
  change its inputs).
- **MEDIUM** — INV-6 reworded into an explicit security gate (zero
  `CsiDispatch` over the full stream, checked after `J`) + a positive
  corroborator (`2`/`J` Print), so the falsification window is exact.
- **MEDIUM** — § 5's C1 scope-out reason "no known emitter" was wrong
  (8-bit-C1 emitters are first-classed at `vtparser.cpp:274-280`).
  Rewritten: all-8-bit back-to-back already works via the 8-bit-ST →
  Ground path; only mixed-encoding streams lose the intro.
- **LOW** — § 4 per-segment-cap claim scoped to OSC/DCS/APC
  (`IgnoreString` has no accumulator); § 6 now names the `test_vt`
  bundle and adds a raw-`0x9C`-ST terminator case; § 2.1 binds
  `X`→SOS / `^`→PM; INV-5's `(` rationale clarified.

No CRITICAL across either loop. A loop 3 (cold) is owed to confirm a
zero-finding pass before sign-off.

**Loop 3 (2026-06-01)** — same two cold reviewers, no briefing.
Zero CRITICAL, zero HIGH; the security reviewer found zero MEDIUM and
re-confirmed the boundary (allowlist exact, `[`/finals/intermediates/C0
all discarded, Kitty-accumulator parity grounded). Folded:

- **MEDIUM (cross-doc)** — the `ROADMAP.md` ANTS-1777 bullet still
  proposed the unsafe `transition(Escape)` re-arm. Annotated the bullet
  (it now points here and flags that approach as superseded) and added
  the supersession note to § 7.
- **LOW** — INV-5 now notes a trailing charset-designator byte Prints
  from Ground; § 6 requires the test to accept both scalar/SIMD Print
  shapes; § 4 DoS note extended to the `IgnoreString` (SOS/PM) chain.

Remaining open items are code-side confirmations for the implementation
test pass (no `IgnoreString` accumulator regression; no peek-state-
specific logic beyond the four cases), not doc fixes.

**Loop 4 (2026-06-01)** — same two cold reviewers. Zero CRITICAL/HIGH;
the security reviewer cleared the spec outright ("proceed to
implementation") after a full byte-class sweep of the allowlist. Two
MEDIUM, both citation-precision, folded: the `0x9C`-ST close cite
`:529` (DCS-only) widened to `:483`/`:529`/`:565` (OSC/DCS/APC) in § 5
and § 6; the Kitty-accumulator cite `:3244` widened to the
`:3244-3277` block (the named symbols start at `:3245`). LOW: INV-6 now
notes the second `ESC` is `o1`'s own terminator. No design findings;
the security boundary and every load-bearing citation verified against
current source by both reviewers.

**Loop 5 (2026-06-01)** — clean pass. Zero CRITICAL/HIGH; both
reviewers verified every citation against current source and the
security reviewer cleared it ("verified clean for implementation"). No
new design or accuracy findings. Folded the residual polish: INV-5's
`(` note now says it is a peek-state non-introducer discard (never
enters `EscapeIntermediate`); INV-7 payload marked illustrative
(`<body>`); § 6 adds an `ESC X … ESC X` SOS/PM re-arm case so the
`X`/`^` entries are tested not just asserted; the ROADMAP annotation's
loop-count synced. Convergence: design clean since loop 2; two
consecutive security-clean passes (loops 4–5). Ready for sign-off.
