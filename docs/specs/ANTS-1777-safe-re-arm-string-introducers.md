# ANTS-1777 — Safe re-arm of string introducers after a string-terminating `ESC`

**Status:** accepted + implemented 2026-06-01 (cold-eyes loops 1–5,
clean pass; signed off). Ships next release. Helper
`VtParser::reArmStringIntroducer`; test `tests/features/vt_string_rearm/`
(7 cases) in the `test_vt` bundle; full suite 1801/1801 green.
**Kind:** fix.
**Source:** ROADMAP.md ANTS-1777 (indie-review #5 2026-05-21; deferred 2026-05-22 pending this security-reviewed spec).
**Pairs with:** `tests/features/vt_osc_esc_discard/spec.md` (the 0.7.53 hardening this must preserve).

**Layman:** Terminals that draw inline images (Sixel, Kitty graphics)
send picture data back-to-back; today the app drops a byte between two
such chunks, so the second picture renders as garbage. This fixes that
without weakening the guard that stops a hostile remote shell from
resetting your terminal.

## 1. Problem

The VT parser's string states (`OscString`, `DcsString`, `ApcString`,
`IgnoreString`) treat a mid-stream `ESC` (0x1B) as the string
terminator: they dispatch the string's End action and transition to a
peek-state (`OscStringEsc` / `DcsStringEsc` / `ApcStringEsc` /
`IgnoreStringEsc`) — `src/vtparser.cpp:511`, `:545`, `:581`, `:599`.

Each peek-state then **unconditionally discards the next byte** and
returns to Ground (`vtparser.cpp:525`, `:561`, `:592`, `:604`). This
was deliberate 0.7.53 security hardening:
a hostile string body ending in `ESC c` must not dispatch RIS (full
terminal reset), and `ESC D` / `ESC M` / `ESC 7` / cursor-control must
not be dispatched either — the string body is attacker-reachable (SSH,
trigger-rule output, paste). See `vtparser.cpp:537-562` and
`tests/features/vt_osc_esc_discard/spec.md`.

The cost (called out in the code comment at `vtparser.cpp:558-560`):
the byte after the terminating `ESC` is the *introducer* of an
immediately-following string. Back-to-back sequences therefore lose
their second introducer:

1. `ESC P …data1… ESC P …data2…` — Sixel graphics. The second `P` is
   eaten; `…data2…` spills into Ground as printable text.
2. `ESC _ …data1… ESC _ …data2…` — Kitty graphics (APC). Same loss.
3. `ESC ] …osc1… ESC ] …osc2…` — back-to-back OSC. Same loss.

Real emitters chain these (animation frames, multi-chunk Kitty image
transmits, paired OSC updates), so the loss is a visible correctness
bug, not a theoretical one.

The fix must restore back-to-back string sequences **without**
re-opening the `ESC c` / dispatch-final hole.

## 2. Surface

### 2.1 The safe-byte insight

The Ground→`Escape` dispatch (`vtparser.cpp:299-335`) splits the byte
after `ESC` into two disjoint classes:

- **String introducers** — `]`→OSC, `P`→DCS, `_`→APC, `X`→SOS, `^`→PM
  (`vtparser.cpp:302-319`). Each re-enters a payload-*collecting* state
  that runs until its own terminator. None has a single-byte side
  effect; none moves the cursor, resets the terminal, or changes a
  mode. Re-entering one grants no capability beyond what a plain
  `ESC ]`/`ESC P`/… already grants in Ground.
- **Everything else** — the `0x30..0x7E` ESC finals (`c`=RIS, `D`=IND,
  `M`=RI, `7`/`8`=DECSC/DECRC, `=`/`>`=keypad, charset designators),
  the CSI introducer `[`, intermediates `0x20..0x2F`, and C0 bytes.
  These are exactly the dispatch paths the 0.7.53 hardening neutralised.

So a peek-state can safely re-arm **iff** the next byte is a string
introducer, and must keep discarding for every other byte. `[` (CSI)
is **excluded** — re-arming it would re-open the cursor-positioning
injection vector the original hardening called out.

**Capability parity (not a new hole).** A re-armed string dispatches
the *same* `OscEnd` / `DcsEnd` / `ApcEnd` a Ground-entered one does, so
its handler (`TerminalGrid::handleOsc`, `src/terminalgrid.cpp:1016` —
OSC 52 clipboard, OSC 8 hyperlink, OSC 9/133/1337) sees identical
input. Re-arm therefore grants no per-sequence capability beyond what a
plain `ESC ]` / `ESC P` already reaches in Ground; it only restores the
*back-to-back* form. Chaining N re-armed strings is equivalent to N
`BEL`/`ST`-separated strings — which already dispatch today (the
hardening only ever neutralised the trailing-ESC *final*, never the
string body). Handlers consume discrete End actions, not parser-state
transitions. The one handler that *does* carry cross-sequence state —
`handleApc`'s Kitty chunk accumulator (`m_kittyChunkBuffer` /
`m_kittyChunkId`, the `more == 1` merge block at
`src/terminalgrid.cpp:3244-3277`, which stitches a multi-chunk
`m=1 … m=0` transfer) — keys its merge on the APC payload's
`m=`/`i=` params, **not** on a Ground visit between sequences. So
whether two APCs are `ST`-separated (Ground visited between them) or
re-armed (no Ground visit), the handler receives the same two `ApcEnd`
payloads and merges them identically; re-arm is transparent to it. The
identical argument holds for every other handler (none keys on Ground).
INV-3 locks the OSC-path parity at the action level.

### 2.2 Implementation

One new private helper in `src/vtparser.{h,cpp}`, shared by all four
peek-states (the new string's identity depends only on the introducer
byte, not on which string we came from):

```cpp
// vtparser.h — private
// ANTS-1777: after a string-terminating ESC, re-enter a string-
// collecting state iff `ch` is a *string introducer* (OSC/DCS/APC/
// SOS/PM). Returns true and performs the transition; false (no state
// change) for every other byte so the caller discards it to Ground.
// Preserves the 0.7.53 ESC-final hardening (c=RIS, D=IND, CSI, … stay
// discarded). 7-bit introducers only — an 8-bit C1 intro after a
// 7-bit ESC terminator is pathological and out of scope.
bool reArmStringIntroducer(uint32_t ch);
```

```cpp
// vtparser.cpp
bool VtParser::reArmStringIntroducer(uint32_t ch) {
    switch (ch) {
    case ']': m_oscString.clear(); transition(OscString); return true;
    case 'P': m_dcsString.clear(); transition(DcsString); return true;
    case '_': m_apcString.clear(); transition(ApcString); return true;
    case 'X':
    case '^': transition(IgnoreString); return true;
    default:  return false;
    }
}
```

Each peek-state collapses to:

```cpp
case DcsStringEsc:   // and OscStringEsc / ApcStringEsc / IgnoreStringEsc
    if (!reArmStringIntroducer(ch))
        transition(Ground);   // discard — preserves 0.7.53 hardening
    break;
```

The introducer-clear semantics mirror the Ground→`Escape` cases
verbatim (`m_oscString.clear()` etc.) so a re-armed string starts with
a clean buffer exactly as a Ground-entered one does.

## 3. Invariants

- **INV-1** — Back-to-back DCS re-arms: feeding `ESC P d1 ESC P d2 ST`
  dispatches **two** `DcsEnd` actions with payloads `d1` and `d2`
  intact (no Print of `d2`). *Test:* `vt_string_rearm` feature test.
- **INV-2** — Back-to-back APC re-arms: `ESC _ a1 ESC _ a2 ST` → two
  `ApcEnd`, payloads intact. *Test:* same.
- **INV-3** — Back-to-back OSC re-arms: `ESC ] o1 ESC ] o2 BEL` → two
  `OscEnd`, payloads `o1`/`o2` intact. *Test:* same.
- **INV-4** — Cross-type re-arm: `ESC ] o1 ESC P d1 ST` → one `OscEnd`
  then one `DcsEnd`. *Test:* same.
- **INV-5** — Security preserved. A string ending in `ESC` + a
  dangerous selector still discards it and emits zero `EscDispatch`:
  for each of `c D M 7 8 = >` (single-byte finals) **and** `(` (which
  in Ground opens a 2-byte charset designation `ESC ( B` — discarded
  here not as a final but as a non-introducer), `ESC P payload ESC <b>`
  yields exactly one `DcsEnd` and zero `EscDispatch`, parser in Ground.
  (In the peek-state `(` is treated as any non-introducer — discarded
  to Ground; it never enters `EscapeIntermediate`. A following
  designator byte like `B` then simply `Print`s from Ground, parallel
  to INV-6's `2`/`J`.) *Test:* `vt_string_rearm` (new) **and** the unchanged
  `vt_osc_esc_discard` INV-1 (full `c D M 7 8 = >` set) — its INV-3
  covers the DCS path for `c` only.
- **INV-6** — CSI stays discarded. Feed the **full** stream
  `ESC ] o1 ESC [ 2 J` (the second `ESC` is itself `o1`'s terminator;
  no trailing `ST`/`BEL` is needed). Two assertions: (a) the security gate — **zero
  `CsiDispatch`** over the entire stream (a `[`-allowlisting regression
  re-enters `CsiEntry` and emits the `CsiDispatch` only when its final
  `J` arrives, so the count must be checked *after* feeding `J`); (b)
  the positive corroborator — `2` and `J` arrive as `Print` (in the
  correct discard path they fall through to Ground as text; under the
  regression they would not). One `OscEnd`. *Test:* `vt_string_rearm`.
- **INV-7** — ST still consumed. The legitimate ST `ESC \` after a
  string is still consumed without re-arming (`\` is not an
  introducer): `ESC ] <body> ESC \` → one `OscEnd`, zero `EscDispatch`,
  no Print of `\`, parser in Ground. *Test:* unchanged
  `vt_osc_esc_discard` INV-2.

## 4. RAM / build cost

No new members, buffers, state-enum entries, translation units, or
external libraries. The helper reuses the existing `m_oscString` /
`m_dcsString` / `m_apcString` accumulators (each already 10 MB-capped).
Re-arming is ordinary sequence processing; chained strings carry no
extra retained state. Memory budget: zero net change. Each re-armed
OSC/DCS/APC segment gets a fresh 10 MB cap (`transition()` resets
`m_stringTruncated` for those three states, `vtparser.cpp:619-623`;
`IgnoreString` carries no accumulator, so the cap is moot there). An
`ESC P x ESC P x …` chain is therefore bounded per-segment and is
DoS-equivalent to the `BEL`/`ST`-separated sequence spam already
reachable today — no new exhaustion vector. An `ESC X ESC X …`
(SOS/PM) chain is likewise bounded: `IgnoreString` retains nothing.

## 5. Out of scope

- **8-bit C1 introducers after a 7-bit ESC terminator** (e.g. a 0x90
  DCS intro right after a string-terminating `ESC`). The allowlist is
  7-bit only, so such a byte is discarded in the peek-state. This is
  not a real loss: a *consistent* 8-bit emitter terminates with the
  8-bit ST `0x9C`, which closes the string straight to Ground
  (`vtparser.cpp:483`/`:529`/`:565` for OSC/DCS/APC), where the next C1
  intro re-enters its string
  state via the Ground C1 path (`vtparser.cpp:274-280`) — so all-8-bit
  back-to-back already works. Only a stream that *mixes* a 7-bit ESC
  terminator with an 8-bit C1 introducer mid-sequence loses the intro,
  and no emitter mixes encodings that way. Not tracked.
- **Re-arming `[` (CSI) or ESC finals.** Deliberately excluded for
  security (INV-5/INV-6); not a deferral.

## 6. Tests

Feature test: `tests/features/vt_string_rearm/` (new). Covers
INV-1..INV-6, including at least one `ESC X … ESC X` (SOS/PM) re-arm
case so the `X`/`^` allowlist entries are falsifiably exercised, not
just asserted. INV-7 is covered by the unchanged `vt_osc_esc_discard`
suite (its INV-2 stream `\x1B]52;c;dGVzdA==\x1B\\`). Label `features;fast`. In-process scalar `VtParser` with a
recording callback (same harness shape as `vt_osc_esc_discard`). The
new test source is added to the **`test_vt`** GoogleTest bundle's
`SOURCES` list in `CMakeLists.txt` (the bundle that already holds
`vt_osc_esc_discard` — not a standalone `add_executable`, per
`tests/features/README.md`). At least one re-arm case (INV-1 or INV-3)
should use the raw 8-bit ST `0x9C` as the terminator (not only
`ESC \` / `BEL`) to confirm the C1-ST close path
(`vtparser.cpp:483`/`:529`/`:565` for OSC/DCS/APC) still ends a
re-armed string. The `Print`
assertions (INV-6's `2`/`J`) must accept **both** scalar Print shapes —
`codepoint` and `printRun`/`printRunLen` — mirroring
`vt_osc_esc_discard`'s harness (`test_vt_osc_esc_discard.cpp:174-190`),
so they don't false-fail on the SIMD path. Verify each new case
**fails against pre-fix source** (today the second introducer is lost /
dispatched to Ground) before applying the helper.

The `vt_string_rearm` test's own `INV-N` ids are **spec-local** to that
feature contract and do not map 1:1 onto this design spec's `INV-N` ids
(nor onto the existing `vt_osc_esc_discard` test, which also numbers its
cases `INV-1..5`). Cross-reference by the prose claim, not the number.

## 7. Cross-doc impact

- `tests/features/vt_string_rearm/spec.md` — the feature-test contract
  (companion to this design spec).
- `src/vtparser.cpp` peek-state comments — the existing `DcsStringEsc`
  / `ApcStringEsc` notes (`:552-560`, `:589-592`) already say "a safe
  partial re-arm … needs its own security-reviewed spec"; replace that
  with a pointer to this (now-written) spec and the shipped re-arm.
- CHANGELOG.md — Fixed entry on ship.
- `ROADMAP.md` ANTS-1777 bullet — its headline proposes "Re-arm via
  `transition(Escape)` (xterm-faithful) or document the limitation."
  That approach is **superseded** by this spec: a blanket
  `transition(Escape)` re-opens the `ESC c`→RIS hole (the whole reason
  for the 0.7.53 hardening). The allowlist design here is the safe
  alternative. Bullet annotated to point here; its stale `:519`/`:544`
  cites are refreshed on ship.
- No CLAUDE.md / PLUGINS.md surface change (no public API / Lua change).

## 8. Cold-eyes loop log

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
