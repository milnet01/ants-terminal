# ANTS-1873 — Single-source-of-truth Claude state resolver — review loop log

Record for [`../specs/ANTS-1873-single-source-truth-claude-state.md`](../specs/ANTS-1873-single-source-truth-claude-state.md) § Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-05-26) — 1 reviewer (single-spec lane), no fix-briefing

Findings folded:

- **C1 (CRITICAL)** — INV-11 vs INV-6 contradiction (the helper *does*
  change behaviour in the `NotRunning + awaitingInput` case; the prior
  `apply()` hid on NotRunning before the prompt check). → INV-6
  amended to call out the intentional improvement; INV-11 re-scoped
  to "equivalent where base != NotRunning" + § 1 adds the
  "Pre-running prompt" sub-case so the improvement is documented.
- **H2 (HIGH)** — wrong setter-line citations (the listed lines were
  `markShellAwaitingInput` calls, not `setPromptActive` calls). →
  § 2.2 now cites the correct `mainwindow.cpp:2260/2276/2334/2343/
  5190/5237/5238` setter sites + `claudestatuswidgets.cpp:466`, with
  the tracker-call sites listed separately as the *paired* updates;
  the unpaired `5190/5237/5238` are called out as harmless after the
  refactor (tracker autonomous transcript watch).
- **H3 (HIGH)** — § 1 and § 2.2 cited different setter lines. → both
  unified on the correct set.
- **H4 (HIGH)** — § 6 asked for a cross-link from a
  `claudetabtracker` subsystems entry that does not exist. → § 6
  amended; the missing entry surfaced as an Open question.
- **M5 (MEDIUM)** — "joins `test_claude`" wording. → reworded to
  "joins `test_claude`'s SOURCES list" with the correct line anchor.
- **M6 (MEDIUM)** — spec was silent on where the tool-vocabulary
  mapping (read→"reading a file", etc.) lives after the refactor. →
  § 2.2 adds an explicit bullet: stays in `apply()` keyed on `r.tool`
  when `d == ToolUseGeneric`.
- **L11 (LOW)** — `resetForTabSwitch` line range `:641-651` was
  inclusive of widget-hide lines. → narrowed to `:642-646`.
- **L12 (LOW)** — out-of-band "user's follow-up suggestion" reference
  (no anchor for future readers). → dropped.
- **L13 (LOW)** — Cold-eyes loop log missing. → this section.
- **L8 (LOW)** — invariants lacked an INV → test-function map. →
  added under § 4.
- **L10 (LOW)** — INV-3/INV-4 had redundant "collapses to Hidden"
  sentences after the `iff`. → dropped (the `iff` already implies
  it); INV-6 kept its expanded form because that one *is* the
  improvement note.
- **L9 (LOW)** — multi-line `Source:`. → compressed to one line; the
  bidirectional clarifier moved to § 1.

Rejected (verified-wrong, not actioned):

- **M7 (MEDIUM)** — claim that `pollAllShells` does not exist. The
  symbol is real: declared at `src/claudetabtracker.h:101`, slot
  connected to the 2 s poll timer at `claudetabtracker.cpp:14-15,138`.
  Reviewer was wrong; spec unchanged on this point.

Verified solid by reviewer (kept):

- H1 + header block conform to `docs/standards/specs.md`.
- Numbered structure (Problem → Surface → Invariants → Tests → RAM →
  Cross-doc) matches the standard.
- `src/claudestatuswidgets.cpp:197-240` (tab-dot lambda),
  `:1019-1128` (`apply()`), `claudestatuswidgets.h:187-191` (cached
  scalars), `claudetabtracker.h:33-52` (`ShellState`),
  `claudeintegration.h` `ClaudeState` enum — all citations accurate.
- `ants_claude_lib` placement is sound.
- Grep confirms `m_lastState` / `m_lastDetail` / `m_promptActive` /
  `m_planMode` / `m_auditing` have no external readers (deletion is
  safe).

### Loop 2 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **H1 (HIGH)** — setter↔tracker pairing was mis-cited at the line
  level (the lambda's tracker-clear is at `mainwindow.cpp:2262`, not
  `:2261`). → § 2.2 re-rendered as a logical-pair list (PermissionRequest
  hook slot / scroll-scan detect / scroll-scan resolve / question
  detect / question clear) with each pair anchored to its setter +
  tracker lines, so an implementer doesn't follow stale per-line
  citations.
- **M2 (MEDIUM)** — `pollAllShells` only re-detects the Claude PID; the
  transcript re-parse is `reparseTranscript`, driven by the file
  watcher (and `pollAllShells`'s `detectClaudeChild` calls
  `reparseTranscript` only on the binding-transition paths, not every
  tick). → § 1 + § 2.2 corrected; the deletion-is-safe argument now
  rests on the watcher path with the poll as the PID-binding signal,
  not on a polled transcript reparse.
- **M3 (MEDIUM)** — red-gate recipe missing for a bundled test. → § 4
  adds the gtest-filter recipe
  (`./build/test_claude --gtest_filter='ClaudeStateResolver.*'`).
- **M4 (MEDIUM)** — `claudestateresolver.cpp` needs to `#include
  "terminalwidget.h"` (forward-decl in the header is insufficient to
  call `focused->shellPid()`). → § 2.1 calls this out.
- **L8 (LOW)** — `apply()` body line range cited as `:1019-1128`; the
  function signature begins at `:1017`. → § 1 narrowed to `:1017-1128`.
- **L9 (LOW)** — CMakeLists vague reference. → § 2.1 cites
  `CMakeLists.txt:340-356`.
- **L10 (LOW)** — pair-with on a not-yet-accepted spec. → header
  annotated "(spec draft)".
- **L11 (LOW)** — § 6's "INV-2 is unaffected" phrasing read as if
  INV-2 contained a code-line citation it does not. → reworded to
  attribute the call shape to the gate, not the invariant.

Open questions surfaced this loop (resolved in subsequent edits):

- Whether the new `shellStateChanged` connect is a new connect or
  amends the existing one. § 2.4 now says explicitly **new** (distinct
  from the `:248-281` tab-bar repaint connect), settling that.
- "First focus tick on a fresh tab may still show the prior tab's
  overlay for one paint until `reparseTranscript` runs." Verified by
  inspection: after the refactor `apply()` reads the new focused tab's
  tracker entry — which is *its own* per-shell state, not the prior
  tab's. The new tab's overlay therefore reflects ITS transcript at
  whatever freshness the watcher last achieved; the bar and dot agree
  on that staleness (no desync). No spec change.

### Loop 3 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **H1 (HIGH)** — § 2.2's "PermissionRequest hook slot — same lambda body"
  was misleading: the set-side (`m_promptActive = true` at `:466-467`)
  lives in the `showPermissionPrompt` helper, while the only same-lambda
  pair is the resolve-side (`clearPromptActive` lambda at `:431-438`).
  → § 2.2 split into "hook detected" (slot at `:386` + helper at
  `:466-467`, distinct scopes) and "hook resolved" (same-lambda pair at
  `:433-434` setter + `:437` tracker).
- **M1 (MEDIUM)** — § 1 still cited `mainwindow.cpp:2261` for the
  tracker false-clear after Loop 2 fixed § 2.2's `:2262`. → § 1
  reconciled.
- **M2 (MEDIUM)** — vocab-mapping range `:1076-1101` covered the input
  variables but the actual map body runs to `:1103`. → § 2.2 retargets
  to `:1078-1103` (Bash + inner if/else chain).
- **M3 (MEDIUM)** — INV-7's `_` placeholders read as C++ tokens. →
  replaced with `anyPid` / `anyTerm` so the contract reads
  unambiguously.
- **L1 (LOW)** — § 2.4's "scalar writes become dead" could mislead a
  reader into expecting the whole `resetForTabSwitch` body to vanish.
  → annotated: only `:642-646` clears go; widget hides + bg-tasks /
  tasks / model-chip resets at `:647-664` remain (superseded —
  Loop 7 narrowed the range to exclude the trailing `apply()` at
  `:665`).
- **L4 (LOW)** — Loop 2 log used "Unverified (recorded, not actioned)"
  for the rejected M7 finding; cleaner as "Rejected (verified-wrong)".
  → relabelled.

Verified solid by reviewer (kept):

- All citation lines hand-verified: `:197-240`, `:248-281` (tab-bar
  repaint connect at `:249`), `:285-302`, `:431-438`, `:642-646`,
  `:1017-1128`, `:1023-1027`, `:1039`; `claudestatuswidgets.h:187-191`;
  `claudetabtracker.h:33-52,98,101`; `claudetabtracker.cpp:14-15,138-147`;
  `claudeintegration.h:47-53`; `CMakeLists.txt:340-356,349,982`;
  every mainwindow.cpp pairing line. Structure conforms to
  `docs/standards/specs.md` § 3. INV → test-function map present.
  Red-gate recipe present. Cross-doc impact identifies the
  `claudestatuswidgets` lane line range correctly.

### Loop 4 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — Cold-eyes log was out-of-chronology (Loop 3 placed
  before Loop 2). → reordered to Loop 1 → 2 → 3 → 4.
- **M2 (MEDIUM)** — § 2.2 preamble said "five prompt-path setter call
  sites" but the table enumerated more than five. → dropped the count;
  table is now the source of truth.
- **M3 (MEDIUM)** — INV-10's test surface promised "behavioural test"
  but the actual `Inv10ShellStateChangedTriggersApply` test is
  source-grep only. → INV-10 rescoped to source-grep coverage; a
  live-controller behavioural variant is named as deferred.
- **L1 (LOW)** — leftover `:1076-1101` narrative cite for the vocab
  map. → unified on `:1078-1103`.
- **L4 (LOW)** — § 6 said the ANTS-1735 INV-2 call-shape switch "can"
  happen with no commitment owner. → reworded to attribute the switch
  to ANTS-1735's actuator-wiring landing, not a separate ROADMAP item.

### Loop 5 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — INV-11's "intentional improvement" carve-out for
  `NotRunning + awaitingInput` read as if both surfaces changed
  behaviour. → INV-11 split into tab-dot (no behaviour change — the
  pre-fix lambda already checked `s.awaitingInput` first at
  `claudestatuswidgets.cpp:208-216`) and status-bar (the one
  intentional change) clauses, so the equivalence claim is precise
  per surface.

Rejected (verified-wrong, not actioned):

- **L1 (LOW)** — claim that `claudestatuswidgets` lane in
  `docs/subsystems.md` runs `:134-139`. Verified the entry ends at
  `:138` ("watch-loss recovery."), with `:139` starting the
  `modelrecommender` bullet. Spec's `:134-138` is correct.
- **M2 (MEDIUM)** — convention nit about `:466-467` vs `:466`. The
  spec uses the same `assign + apply()` pair convention everywhere
  (`:433-434` for clear); it is internally consistent. No change.

### Loop 6 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — § 1's "keeps that struct fresh via two paths"
  framing was loose: both paths *can* touch `ShellState`, but only
  the watcher updates `state`/`tool`/`planMode`/`auditing` directly;
  the poll only re-detects the Claude child PID and triggers a
  reparse on the binding-transition. → § 1 tightened to attribute
  field updates to the watcher path and PID detection / binding-
  transition reparse to the poll.
- **L2 (INFO)** — § 6's parenthetical about the missing
  `claudetabtracker` subsystems lane duplicated the § Open
  questions bullet. → § 6 now links to § Open questions instead.

Verified solid by reviewer (kept):

- Every code citation re-verified line-by-line; all match. INV-6 /
  INV-11 per-surface carve-out is internally consistent (verified
  against `claudestatuswidgets.cpp:208-216` tab-dot lambda).
  ANTS-1735 INV-2 alignment confirmed. Red-gate recipe present.
  Layman line + RAM section present. Cold-eyes log in chronological
  order.

Rejected (verified-wrong, not actioned):

- **L1 (LOW)** — suggestion to annotate churned citation lines as
  "verified 2026-05-26" in the loop log. The loop log already
  carries the per-loop verification record; an inline annotation
  would duplicate it. No change.

### Loop 7 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — § 2.3 cited `:248-280` for the tab-bar repaint
  connect; § 2.4 cited `:249-281` for the *same* block. Verified:
  the `if (m_tracker)` opens at `:248`, the `connect(...)` body
  spans `:249-280`, and the `}` closing the if is at `:281`. →
  unified on `:248-281` (the guarded block) across § 2.3 / § 2.4 /
  Loop-2 log entry.
- **M2 (MEDIUM)** — § 2.4's "ends with `apply()`" read as
  introducing a new call; verified `resetForTabSwitch` already
  trailing-calls `apply()` at `claudestatuswidgets.cpp:665`. →
  rephrased: "already ends with `apply()` (`:665`)" — the change is
  the deletion of the cached-scalar writes at `:642-646`, not a new
  `apply()` call. Also corrected the "widget hides … remain" line
  range to `:647-664` (line 665 is the existing `apply()`).

Verified solid by reviewer (kept):

- "Spec is implementation-ready. Findings are documentation-clarity
  issues, not contract gaps." (Reviewer's own conclusion.) Every
  remaining citation re-verified.

### Loop 8 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — § 2.2's bullet list ran paired + unpaired sites
  in one stream; an inattentive reader could miss that
  `mainwindow.cpp:5190/5237/5238` are deliberately unpaired. → § 2.2
  split into explicit "Paired" + "Unpaired" sub-lists.
- **M2 (MEDIUM)** — spec didn't justify
  `m_focusedTerminalProvider` over `m_currentTerminalProvider` for
  `apply()`'s read; picking the wrong provider would silently
  re-introduce the per-tab desync. → § 2.2 adds a "Provider choice"
  paragraph naming the correct provider, the precedent
  (`refreshModelChip` at `:1140`), and what `m_currentTerminalProvider`
  is reserved for.
- **L1 (LOW)** — INV-5's case-insensitivity is a unification *change*
  from the live tab-dot lambda's case-sensitive Bash check at
  `claudestatuswidgets.cpp:232`. → INV-5 now calls this out
  explicitly so an implementer doesn't "preserve" the case-sensitive
  comparison.
- **L2 (INFO)** — § 1's first `apply()` citation still read
  `:1019-1128` after Loop 2 harmonised the rest on `:1017-1128`. →
  unified.
- **L3 (INFO)** — INV-10's parenthetical "a controller test fixture
  could exercise it" was redundant after Loop 4 rescoped the test to
  source-grep. → moved to a stand-alone "Live-controller behavioural
  coverage deferred" line so the contract is unambiguous.

Reviewer's own verdict (loop 8): "Spec is implementation-ready.
Findings are documentation-clarity issues, not contract gaps."

### Loop 9 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **H1 (HIGH)** — § 2.2's "TUI scroll-scan prompt resolved" row had
  the line citations in reverse fire order (`:2262` tracker / `:2260`
  setter, but the setter fires first at `:2260` then the tracker at
  `:2262`). → reordered to "`:2260` setter, `:2262` tracker — fire
  order".
- **M1 (MEDIUM)** — § 2.2's "Paired with a tracker update in the
  same block" header conflicted with the first bullet's "Slot +
  helper, distinct function scopes" annotation. → renamed to
  "Paired (setter + tracker fire together in the same control flow)".
- **M2 (MEDIUM)** — Loop log out of chronological order again: Loop 8
  inserted before Loop 7. → restored chronology (Loop 1 → 2 → 3 → 4
  → 5 → 6 → 7 → 8 → 9).
- **L1 (LOW)** — INV-7 listed `forFocused(nullptr, …)` and
  `forFocused(t, nullptr)` but not the third case
  `forFocused(t, term-with-shellPid≤0)` that § 2.1 names. → INV-7
  amended with the third case.

Verified solid by reviewer (kept):

- Every cited file:line re-verified line-by-line. INV-5 case-insensitive
  unification, provider-choice justification, RAM/build cost, red-gate
  recipe, Layman line, INV → test-function map — all present and
  accurate.

### Loop 10 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **H1 (HIGH)** — Loop-3 fold still cited `:647-672` for the
  "remain" range; Loop 7 narrowed it to `:647-664` everywhere else.
  → Loop-3 entry annotated as superseded.
- **M1 (MEDIUM)** — § 2.2 described the `clearPromptActive` lambda
  as a "single body" pair but the pre-fix lambda guards the setter
  on `belongsToFocused` while the tracker call is unconditional. →
  the asymmetry now noted explicitly with a one-line carve-out.
- **M2 (MEDIUM)** — `claudestate::display()` doc-comment in § 2.1
  only restated the ladder order; reader could miss INV-3 / INV-4's
  `base != NotRunning` collapse. → doc-comment now cites INV-3/INV-4
  inline.

Polish items deferred (low / info, non-blocking; surfaced for the
record):

- **L1 (LOW)** — `Source:` header on two lines (the standard prefers
  one, but the form parses fine for `spec_query`).
- **L2 (INFO)** — "near line 982" vague; could pin to `:983`.
- Open question on where exactly the new `shellStateChanged →
  apply()` connect is wired (inside `attach()`, alongside the
  existing tab-bar repaint connect at `:248-281`) — narrative-level,
  implementer can pick the obvious spot.

### Loop 11 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — Loop 10's "tracker call is unconditional, gated
  only by `m_tracker && awaitingPid > 0`" was self-contradictory
  ("unconditional" + a guard). Both the set-side and clear-side
  tracker calls share the same `m_tracker && awaitingPid > 0`
  guard; only the **setter** is wrapped in `if (belongsToFocused)`.
  → reworded to state the symmetry precisely.
- **M2 (MEDIUM)** — § 2.2 called the scroll-scan detect pair
  ("`:2256` tracker, `:2276` setter") "adjacent statements in the
  same slot"; verified the two are ~20 lines apart, separated by
  the `clearPromptActive` lambda + button-connect block. →
  rephrased to acknowledge the gap.
- **M3 (MEDIUM)** — § 2.2 Unpaired list conflated `:5190` (fires
  for **every** tab change including teardown) with `:5237/5238`
  (inside `if (t exists)` branch). The "harmless because new tab's
  tracker is authoritative" justification didn't cover the
  `t == nullptr` teardown case for `:5190`. → split into two
  sub-cases: (a) `:5190` harmless via INV-7
  `forFocused(t, nullptr) → Resolved{NotRunning}` → Hidden; (b)
  `:5237/5238` harmless because the new tab's tracker entry is
  authoritative.

Polish items deferred (low / info, non-blocking; surfaced for the
record):

- **L1 (LOW)** — `Source:` header on two lines (carry-over from
  Loop 10) — **fixed in Loop 12**.
- **L2 (INFO)** — "near line 982" → `:983` (carry-over from Loop 10)
  — **fixed in Loop 12**.
- **L3 (INFO)** — Status line could mark a "clean pass" milestone
  when one finally lands.

### Loop 12 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded (carry-over polish + one new clarity item):

- **H1 (HIGH)** — § 2.1 was silent on whether consumers
  (`claudestatuswidgets.cpp`, future `modelautoswitch` callers)
  need to include `terminalwidget.h` to use `forFocused`. → § 2.1
  now says explicitly: only `claudestateresolver.cpp` includes it;
  consumers use the forward decl.
- **M1 (MEDIUM)** — `Source:` line collapsed to a single line
  (deferred since Loop 9; promoted at Loop 12 to clear the
  carry-over).
- **M2 (MEDIUM)** — § 4 "near line 982" → `:983` precise anchor
  (deferred since Loop 10; promoted at Loop 12).
- **L1 (LOW)** — § 2.2 "PermissionRequest hook detected" bullet
  named "the helper" without identifying it. → restored
  `showPermissionPrompt` anchor.
- **L3 (INFO)** — § 2.4 didn't say where the new
  `shellStateChanged` connect is wired. → now states explicitly
  "inside `ClaudeStatusBarController::attach`, alongside the
  existing tab-bar repaint connect at `:248-281`".

Polish items remaining (info-only):

- **L2 (INFO)** — Could widen the vocab-map citation to
  `:1076-1103` to include the input-variable setup at `:1076-1077`.
  Defensible either way; leaving at `:1078-1103` (mapping body
  proper).

### Loop 13 (2026-05-26) — fresh reviewer, no fix-briefing

Findings folded:

- **M1 (MEDIUM)** — § 4's "`:983` precise anchor" claim collided
  with the actual `CMakeLists.txt:983` being occupied by
  `mcp_extra_tools/test_mcp_extra_tools.cpp`. → reworded to
  "appended after `:982`; existing `:983` shifts down" so an
  implementer doesn't overwrite the unrelated test entry.
- **L1 (LOW)** — § 2.1 forward-decl claim now states explicitly
  that TerminalWidget appears only by pointer in `forFocused`'s
  signature (which is why the forward decl is sufficient for
  consumers).

Reviewer's recommendation: stop iterating (Loops 7-12 were polish-
only carry-overs; the loop log is now ~50 % of the spec). The
remaining `L2 (INFO)` vocab-range widening is defensible either way
— not actionable. **Spec is implementation-ready.** The user-facing
behaviour, invariants, helper API, refactor surface, and test
strategy are all locked.
