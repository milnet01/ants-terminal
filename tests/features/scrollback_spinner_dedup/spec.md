# scrollback_spinner_dedup — ANTS-1188

> **Status:** 📝 Draft — blocked on live byte capture from a spinning
> Claude Code session (see §3 *Capture protocol*). The proposed root
> cause in §1 is a hypothesis from the 2026-05-08 ROADMAP entry; the
> fix mechanism in §4 is two options pending the capture. Test
> wiring + acceptance criteria scaffold are in §5/§6.

## 1. Problem

User report 2026-05-08 (screenshot captured): Claude Code's
`Tempering…` / `Sublimating…` busy-spinner lines pile up in
scrollback instead of overwriting in place. The same spinner status
appears 5+ times in succession, each frame showing a different
elapsed time + token count. The LIVE viewport is fine; the
scrollback *history* has the duplicates.

User-paraphrased: *"the spinner shows up many times in the chat
history instead of just updating in place."*

User-impact: cosmetic but visible; scrollback density degrades
sharply during long Claude Code turns. Partially-sighted user finds
the duplication noisy when scrolling back to read.

Distinct from ANTS-1118 (paint-cycle race during live scroll,
shipped 0.7.65) which was about the live view, not scrollback.

## 2. Hypothesis

Claude Code (Ink-based) updates its spinner via a `\n` at bottom-of-
screen pattern rather than `CSI 2J`. When the cursor is at the last
row and Claude emits `\n`, `TerminalGrid::scrollUp()` fires and
pushes the OLD spinner row into scrollback BEFORE the new content
overwrites the now-bottom row. The existing
`m_csiClearRedrawActive` suppression window at `terminalgrid.cpp:
1913` only arms on full-clear sequences (CSI 2J, or CSI 0J / 1J
from the corner) — it never arms on a plain `\n`, so spinner
updates accumulate one row per frame.

**This is a hypothesis.** The actual byte sequence Claude Code
v2.1.138 emits between spinner frames is unverified as of
2026-05-11 and MUST be captured before a fix lands.

xterm comparison: also unverified. The user reports "fixes itself
when I scroll out of view" — that's the symptom (scrollback has the
duplicates; the live view doesn't). It does not tell us whether
xterm shows the same scrollback duplication.

## 3. Capture protocol

Required from the user (or me, when a Claude session is reachable):

1. **Enable VT debug logging** in the terminal:
   - `Tools → Debug Mode → VT parser` (toggle on).
   - OR set `ANTS_DEBUG=vt` in the environment before launch.
2. **Open a Claude Code session in a tab** and trigger a turn that
   spins for ≥ 10 seconds — anything with a tool call that takes
   real time. Common triggers: `read this large file`, `ls -R /`,
   `git log --all`.
3. **Observe the spinner cycling.** When at least 3 spinner frames
   have appeared in scrollback (visible duplicates), copy the
   debug log slice covering:
   - The 2 seconds before the first duplicate appeared.
   - All bytes for the next 5 seconds.
4. **xterm A/B (if available):** repeat the same workflow under
   `xterm` (or `konsole`, `alacritty`, `kitty`) and report whether
   scrollback shows the same duplicates. If a non-Ants terminal
   shows the same duplication, the divergence is in Claude Code's
   output (out of scope for this spec; ANTS becomes a follow-on
   feature flag instead of a fix). If only Ants duplicates, the
   divergence is in our `scrollUp` path.

Capture a `pty_dump` if convenient: `ANTS_PTY_DUMP=/tmp/pty.bin
<launch ants>` — that captures the raw bytes from the PTY before
VT-parsing, which lets us replay deterministically as a test
fixture.

Acceptance for this step: a ≤ 1 KiB byte sequence that, when fed
through a fresh `VtParser → TerminalGrid` pair in a unit test,
reproduces ≥ 2 phantom scrollback entries with identical text.
Without that fixture, the spec stays in DRAFT and no fix lands.

## 4. Fix mechanism — three candidates

The right choice depends on the captured bytes. Each option is
described so the spec can converge once the bytes arrive.

### 4.a Extend the suppression window to cover `\n`-at-bottom patterns

Arm `m_csiClearRedrawActive` (or a sibling flag) when:
- The cursor is at the last row before a `\n` arrives, AND
- A subsequent `cursor-up` or `cursor-position` brings the cursor
  back into the screen within a tight time window (e.g. 50 ms).

Behaviour: suppresses the scrollback push on the `\n` that the
spinner is about to overwrite. Carries the same "user-at-bottom"
guard as the existing window (don't suppress if the user paused at
scrollback — that breaks legitimate scroll-back-to-history flows).

Pros: surgical; preserves intent of existing window mechanism.
Cons: requires recognising the spinner's `\n` → cursor-up pattern
across a small time window; false positives if a TUI does
`echo\necho\necho` at bottom of screen (we'd lose a legitimate
scrollback line).

### 4.b Blank-row elision at push time

In `scrollUp` at the push-to-scrollback decision, fingerprint the
row being pushed and compare to the last N pushed rows. If the row
is identical (or differs only in numeric token counts / elapsed
time), elide the push.

Pros: doesn't care about the input sequence shape; catches any
spinner-style pattern regardless of how it's authored.
Cons: needs a fingerprint definition that distinguishes "spinner
update" from "legitimate repetition" (a `while true; do echo
'Hello'; done` test loop would also get elided). Risky.

### 4.c Tail-merge: detect a row being pushed whose content the
**next** bottom row is about to overwrite

Run the push lazily: hold the would-be-pushed row in a
`std::optional<TermLine> m_pendingScrollbackTail` for one VT-action
batch. If the next batch overwrites the now-bottom row with content
that "supersedes" the held tail (fingerprint-equal-modulo-numeric),
discard the held tail. Otherwise commit it to scrollback as usual.

Pros: correctly handles both legitimate cases (the next batch
writes different content → commit the tail) and spinner cases
(the next batch is the same spinner with updated numbers →
discard).
Cons: most complex of the three; introduces one-batch latency
in scrollback-tail commit, which could surface subtle ordering
bugs.

**Recommended-pending-capture:** start with 4.c if the bytes
confirm the `\n`-at-bottom pattern. Fall back to 4.a if that's too
complex. Avoid 4.b unless the bytes show patterns that 4.a / 4.c
can't catch.

## 5. Invariants (placeholder)

To be finalised once the byte capture arrives and the fix
mechanism is chosen. Expected shape:

| #  | Lane         | Statement |
|----|--------------|-----------|
| 1  | scrollback   | After N consecutive spinner updates at bottom-of-screen, scrollback grows by at most 1 row (not N rows). |
| 2  | scrollback   | A user-driven legitimate scroll-content sequence (`for i in $(seq 5); do echo $i; done` at bottom of screen) MUST push all 5 rows to scrollback — the fix MUST NOT regress legitimate scroll-and-archive flows. |
| 3  | grid         | The live viewport's bottom row reflects the most recent spinner content, byte-for-byte identical to what would render without the fix. |
| 4  | grid         | When the user has paused at scrollback (`m_scrollbackInsertPaused == true`), the fix MUST NOT alter behaviour — the existing guard at `terminalgrid.cpp:1930` continues to apply. |
| 5  | perf         | The fix adds no synchronous work to the hot `scrollUp` path beyond a fingerprint compare (O(row-width) at most). No new allocations per push. |

## 6. Acceptance

`ctest -L features -R ScrollbackSpinnerDedup` exits zero.

Test shape:

1. **Replay fixture** — captured bytes (§3) fed through
   `VtParser → TerminalGrid` (no GUI). Assert scrollback row
   count after N spinner frames is ≤ 1 (not N).
2. **Negative — legitimate scroll** — synthetic byte sequence:
   `for i in 1..5: emit "line {i}\n"` at bottom of screen.
   Assert scrollback grows by 5 (no false-positive elision).
3. **Pre-fix verification** — run the same replay fixture against
   `git stash`-reverted code; assert scrollback grows by N (bug
   reproduces); restore.

Wired into the existing `test_vt` bundle per ANTS-1217 — no new
`add_executable`. The TerminalGrid path is already linked into
`test_vt`; the new test source goes into that bundle's SOURCES.

## 7. Memory budget

Depends on fix mechanism choice:

- **4.a (suppression-window extension):** zero new state. Reuses
  the existing `m_csiClearRedrawActive` + `m_csiClearRedrawTimer`.
- **4.b (blank-row elision):** small fingerprint cache —
  `std::deque<uint64_t>` of the last 4-8 pushed row hashes, ≤ 64 B
  total. Per-grid (one per terminal tab).
- **4.c (tail-merge):** one `std::optional<TermLine>` per grid for
  the held tail. `TermLine` is ~roughly the size of one row's
  cells (~80 × ~16 B = ~1.3 KiB worst case at 80-col 16-byte cells,
  cheaper with the cell pool). Held only across one VT-action
  batch — typically µs of lifetime.

Test memory: the replay fixture is a ≤ 1 KiB byte sequence; the
TerminalGrid is a fresh `40 × 24` instance per test. No fixtures
on disk; inline `QByteArray` literals.

## 8. Cross-references

- **ANTS-1118** (live-viewport paint race during scroll, shipped
  0.7.65) — different symptom in the same neighbourhood.
- **ANTS-1059** (terminal-throughput investigation) — surfaced
  that `scrollUp` is 65 % of `newline_stream` cost. Any fix here
  that reduces scrollback pushes will also help throughput.
- **`m_csiClearRedrawActive`** mechanism at `terminalgrid.cpp:
  1768` / `:1913` — the existing suppression window the fix may
  extend.

## 9. Re-open conditions

If the symptom recurs after this spec lands:

1. **Different byte sequence than captured** — Claude Code may
   have changed its spinner emission style. Re-run §3 capture,
   compare to the original fixture, extend the test corpus.
2. **xterm-divergent flag needed** — if §3.4 A/B shows xterm
   produces the same duplicates, this becomes a "non-conformant
   feature flag" item (opt-in suppression for the spinner
   pattern) rather than a pure fix. Surface to user before
   reopening.
3. **Other TUIs trip the elision** — a future TUI emits a
   spinner-like pattern that legitimately needs scrollback
   archive. The negative-test invariant (§5 INV-2) should catch
   this in CI; if it slips through to the user, capture the
   tripping pattern and refine the fingerprint or trigger
   condition.

## 10. Out of scope

- Anything in Claude Code itself. We treat the spinner emission as
  external behaviour to accommodate.
- Compatibility with non-Ink-based TUI spinners (the YAML/JSON of
  the spinner shape isn't material — what matters is the byte
  sequence).
- Live-viewport rendering (covered by ANTS-1118).
- Throughput improvement target (covered by ANTS-1059, though this
  fix likely contributes).
