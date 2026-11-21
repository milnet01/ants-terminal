# ANTS-1894 — Near-miss telemetry: log gate-hit-but-blocked decisions — review loop log

Record for [`../specs/ANTS-1894-near-miss-telemetry-log-gate.md`](../specs/ANTS-1894-near-miss-telemetry-log-gate.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-05-27) — folded

Three parallel reviewers (lane A: spec vs cited source code; lane B:
spec vs sibling specs + standards; lane C: spec vs CLAUDE.md /
ROADMAP / CHANGELOG / docs/subsystems.md). All HIGH + MEDIUM findings
verified against current files and folded into the spec.

- **HIGH / `evictToCap` line cite wrong** (A-H1, B-H1) — spec cited
  `src/modelswitchledger.cpp` L171–191; actual location is L109–129.
  Folded: § 2.3 eviction paragraph updated; the prose now references
  the real `lines.size() > 1` guard rather than the imaginary
  "size ≤ cap" loop guard.
- **HIGH / Lib placement self-contradicts** (A-H2) — spec said new
  TU lives "in `ants_claude_lib`" but "alongside `modelswitchledger`,
  not `modelautoswitch`", and these are different libs
  (`modelswitchledger.cpp` is in `ants_core_lib` at
  `CMakeLists.txt` L230; `modelautoswitch.cpp` is in
  `ants_claude_lib`). Folded: § 2.3 placement chosen as
  `ants_core_lib` (parallel to firing ledger, reachable from
  `RemoteControl::cmdModelSwitchStats` without cross-lib calls).
- **HIGH / Test-bundle split needed** (A-H3) — INV-5 / INV-6 tests
  drive `ClaudeStatusBarController` (in `ants_claude_lib`); but
  `test_core` only links `ants_core_lib`. Folded: § 4 + § 7
  rewritten to split — 10 pure tests + 1 MCP test on `test_core`;
  2 controller-touching tests on `test_claude`.
- **HIGH / `ClaudeState` enum mis-named** (A-H4) — spec showed JSON
  `"focused_state":"Streaming"` and called the enum 4-state;
  `src/claudeintegration.h` L47–53 defines 5 values, no `Streaming`
  (`NotRunning`, `Idle`, `Thinking`, `ToolUse`, `Compacting`).
  Folded: JSON example uses `"thinking"`; field comment lists all
  5 lowercase snake_case strings; helper § 2.5 spec'd as a 5-case
  switch.
- **HIGH / `claudeStateName` helper fabricated** (A-H5, B-C1) — spec
  asserted "existing string-conversion helper" but source-grep
  returns nothing (`claudestateresolver.cpp:34`'s switch maps to
  glyph enums, a different domain). Folded: § 2.5 rewritten to
  declare the helper as new file-static work, with the canonical
  5-case body inline.
- **HIGH / § 8 CLAUDE.md module-map instruction targets a moved
  feature** (C-H1) — spec asked for an edit to CLAUDE.md's "module
  map brief listing", but per ANTS-1292 that catalogue moved to
  `docs/subsystems.md`. Folded: § 8 drops the CLAUDE.md module-map
  bullet; the `docs/subsystems.md` row is now the sole instruction
  (with proposed bullet text supplied).
- **HIGH / ROADMAP body figures stale** (C-H2, C-H3) — ROADMAP
  bullet body said 30 s throttle and listed 5 reasons; spec sets
  5 s emit-on-sig-change and a 7-token taxonomy. Folded: § 8 now
  prescribes correcting both ROADMAP figures in the
  spec-acceptance commit, citing the canonical INV-9 list and the
  § 2.5 / OQ-1 5 s floor.
- **HIGH / ANTS-1895 line-ref will shift** (C-H4) — ANTS-1895
  bullet cites `claudestatuswidgets.cpp:1425`; § 2.4 removes
  line 1415 ahead of it. Folded: § 8 adds a bullet asking the
  spec-acceptance commit to fix the cite (or annotate as
  pre-1894).
- **HIGH / ANTS-1735 INV-6 amendment annotation** (B-M4) — spec's
  INV-3 raises the dwell floor to
  `max(kMinDwellMs, configuredMinDwellMs)`, amending ANTS-1735
  INV-6. Per `docs/standards/specs.md` § 5.5 the old invariant
  needs an annotation. Folded: § 8 now prescribes appending the
  amendment-by note to ANTS-1735-autonomous-claude-code-model-switcher.md INV-6.
- **MEDIUM / MCP descriptor line range wrong** (A-M2, B-L3) — spec
  said `L2152–2210`; actual closing `tools.append(t);` sits near
  L2228. Folded: § 2.6(3) widened to "~L2152 to L2228".
- **MEDIUM / Memory-doc link path unreachable** (A-M3, C-L3) —
  relative link from `docs/specs/` to a `$HOME` memory file did
  not resolve. Folded: link dropped; memory referenced by file
  name only.
- **MEDIUM / "~12 .cpp files" vs actual 13** (A-M4) — § 4 build
  cost said ~12; § 7 table has 13. Folded: § 4 now lists every
  test file by name with bundle membership; total 13 explicit.
- **MEDIUM / INV-1 clarification on `recommendedTier`** (A-M5,
  B-L4) — added parenthetical citing INV-4 and ANTS-1735 INV-7.
- **MEDIUM / Sibling specs are ROADMAP-only** (B-M1) — folded into
  § 5 (preamble + per-bullet annotation) so a reader doesn't
  follow a dead spec-file link.
- **MEDIUM / `fromJson` contract co-location** (B-M3) — § 2.3
  public API now carries a one-line gloss linking back to INV-13.
- **MEDIUM / `docs/subsystems.md` row text not supplied** (C-M1) —
  § 8 now gives the full proposed bullet text matching the
  em-dash format.
- **MEDIUM / No new config keys not stated affirmatively** (C-M2) —
  § 8 adds an explicit "no change" bullet for the CLAUDE.md
  config-keys block.
- **MEDIUM / MCP descriptor wording not specified** (C-M3) —
  § 2.6(3) now supplies the appended sentence verbatim.
- **MEDIUM / INV-12 omits config triple in `statsFull`** (A-L5) —
  INV-12 rewritten to contract preservation of the ANTS-1889
  config triple + `scope` echo in both modes.
- **LOW / `effectiveMinDwellMs` declaration placement** (A-L3) —
  § 2.1 now states it's a file-static helper in `modelautoswitch.cpp`.
- **LOW / INV-9 test approach used literal `…`** (A-L4) — INV-9
  rewritten with the full 7-token alternation.
- **LOW / INV-2 phrasing muddled** (B-L2) — fixed to
  "table-driven over a representative subset of the 2^7 guard
  combinations".

Reviewer dismissals (verified false / not actionable):
- ANTS-1217 spec exists (✅ shipped) — no fix.
- `bad_mode` code already in `mcp-error-codes.md:38` — no
  standards-doc update needed.
- README.md has no auto-switcher mention — § 8 "No change" stands.
- ANTS-1891 reciprocity asymmetry (B-M2) — pair + out-of-scope
  coexist by design; informational.

### Loop 2 (2026-05-27) — folded

Three parallel reviewers, briefed identically to loop 1 with no
prior-loop fix list (per cold-eyes skill Phase 5 — issues not raised
again prove the loop-1 fixes held). All HIGH + MEDIUM findings
verified against current files before fold-in.

- **HIGH / `refreshAutoModelSwitch` start line wrong** (A-H1) — spec
  cited `L1340–1446`; actual function declaration sits at L1319.
  Folded: § 2.4 now reads "declaration at L1319, body runs to
  L1446".
- **HIGH / `evictToCap` end line off by 3** (A-M3 → re-classified
  HIGH on re-read) — spec cited L109–129 after loop 1; actual close
  is L126. Folded.
- **HIGH / MCP descriptor close line off by 3** (A-M2) — spec cited
  ~L2228 after loop 1; actual `tools.append(t);` is L2225. Folded in
  both occurrences (§ 2.6(3) and § 8 descriptor bullet).
- **HIGH / `tests/features/README.md § Bundles` cite imprecise**
  (B-M1) — actual section title is "Bundles (ANTS-1217)". Folded
  inline via descriptive expansion (see § 7 / § 4 wording — the
  policy now reads with the literal section header).
- **HIGH / Test-bundle prose count mismatch** (A-M1) — § 7's
  "test_core ... 11 pure tests" rephrased to "10 pure ledger/decide
  tests + 1 MCP test (11 total)" so the prose matches the bullet
  list 1:1.
- **HIGH / "ANTS specs § 5.5" malformed citation** (B-H1) — INV-9
  used the casual form; folded to
  "`docs/standards/specs.md` § 5.5 *Invariant immutability*".
- **HIGH / CLAUDE.md paragraph anchor narrow** (C-H1) — § 8 cited
  CLAUDE.md L156–157; the paragraph itself spans L138–157. Folded
  to "paragraph L138–157, ending with the closing sentence at
  L156–157", plus "appends **in-paragraph** (no new paragraph) to
  match the existing mid-paragraph ANTS-1891 sentence at L147".
- **HIGH / ROADMAP correction misses the 2 new tokens** (C-H3) —
  § 8(a) prescribed "correct to INV-9" without naming the two
  tokens the old ROADMAP enumeration omits. Folded: § 8 now calls
  out `auto_switch_disabled` and `dwell_time_insufficient`
  explicitly with their provenance (gate enabled check; configurable
  min-dwell).
- **HIGH / CHANGELOG entries lacked headlines + Layman gloss**
  (C-H4) — § 8 listed topics only; project precedent (ANTS-1891
  block at CHANGELOG L40–63) is bold headline + Layman + technical
  body. Folded: § 8 now sketches all four entries verbatim
  (3 `### Added` + 1 `### Changed`).
- **MEDIUM / ROADMAP sibling line-ref also affects ANTS-1893**
  (A-M4 / C-M5) — § 8 ANTS-1895 bullet only named 1895; ANTS-1893
  also cites `claudestatuswidgets.cpp:1425` at `ROADMAP.md:19442`.
  Folded: § 8 bullet now covers both 1893 and 1895.
- **MEDIUM / `docs/subsystems.md` `claudestatuswidgets` row needs
  update** (C-M2) — its responsibilities now include the near-miss
  emit path. Folded: § 8 subsystems.md bullet now (i) adds the
  `modelnearmissledger` row with body matching siblings' depth,
  (ii) appends a sentence to the `claudestatuswidgets` row noting
  `maybeEmitNearMiss` + the two `QHash` throttle maps.
- **LOW / `kStatsWindowMs` constant unreferenced in pseudo-code**
  (A-L4) — folded: inline comment now reads "24 h cutoff used by
  statsSlim / statsFull".
- **LOW / § 7 intro INV-1..11, INV-13 leaves INV-12 implicit**
  (A-L3) — folded: parenthetical "; INV-12 lives in the MCP dir
  below" added.

Loop 2 reviewer dismissals (verified false / declined with reason):
- **B-H3 / ANTS-1735 INV-7 amendment** — declined as an
  amendment. INV-7's postcondition ("when decide acts, tierArg =
  tierName(clamped)") is bit-for-bit preserved by ANTS-1894 INV-1;
  the new `blockedBy.isEmpty()` precondition is additive, not a
  redefinition. § 8 ANTS-1735-autonomous-claude-code-model-switcher.md bullet now states this explicitly
  so a future reader doesn't re-litigate.
- **B-H2 / Status lifecycle phrasing** — addressed by promoting
  Status to "loops 1 + 2 folded" (matches the ANTS-1891 precedent).
- **A-L1 / wrapping at L31** — informational; spec already cites
  the multi-line range L30–31 in surrounding prose; reader nit not
  worth a re-edit.
- **A-L2 / Status lifecycle** — same as B-H2 above.
- **C-M1 / subsystems.md row depth** — the proposed row is 6 lines
  matching `modelswitchledger`'s 6-line depth (L182–187);
  reviewer's "2 lines" was a markdown-parsing artifact of the
  backtick-wrapped sample. The new C-M2 fold-in rewrote the row
  body to the deeper form anyway, so this finding is subsumed.

### Loop 3 (2026-05-27) — folded

Three parallel reviewers, briefed identically with no prior-loop fix
list. Findings narrowed further — three substantive issues plus a
series of single-line tightenings.

- **MEDIUM / Timestamp precision mismatch** (A-M1) — JSON sample
  showed `.000Z` millisecond precision but `ModelSwitchLedger::nowIso8601`
  returns second-precision `Qt::ISODate` (`src/modelswitchledger.cpp:499–501`).
  Folded: JSON sample uses `"2026-05-27T09:30:00Z"`; § 2.3 Record
  struct comment now names the helper and the precision explicitly.
  Decision: reuse the firing-ledger helper (no new helper, no
  divergence in timestamp format across the two ledgers); the 5 s
  emit-floor + 2 s tick rate make ms precision unnecessary for
  forensic ordering in practice.
- **MEDIUM / CHANGELOG voice template misdescribed** (A-M2) — § 8
  CHANGELOG draft used a `*Layman:*` line, but CHANGELOG.md L40–63
  entries (the prevailing voice) have no `*Layman:*` line — the
  user-facing prose IS the layman gloss, sitting in the technical
  paragraph after the parenthetical `(ANTS-NNNN)`. Folded: all
  four `### Added` / `### Changed` drafts rewritten to the
  user-facing-headline + plain-prose form, no separate Layman
  line.
- **MEDIUM / CLAUDE.md gate-sentence scope** (C-M2) — the existing
  closing sentence "Envelope readers should check
  `measured_downgrades > 0` before treating `regret_rate` as
  meaningful." (CLAUDE.md L156–157) reads as gating the whole
  envelope; ANTS-1894's `near_misses.total_24h` is meaningful
  from the first record and is NOT under that gate. Folded: § 8
  now prescribes (i) re-scoping the gate sentence to explicitly
  apply only to `regret_rate` (adding a parenthetical naming
  `near_misses` as independent), (ii) THEN appending the ANTS-1894
  sentence in-paragraph after the rescoped gate.
- **MEDIUM / ROADMAP body's inline record schema also stale** (C-M4) —
  § 8(a) originally listed two ROADMAP corrections (throttle figure +
  taxonomy list); reviewer surfaced a third: the inline schema uses
  the working-name `ticks_stable` (canonical: `ticks_target_stable`)
  and omits `session_id` + `ms_since_last_override`. Folded: § 8(a)
  now carries a third sub-bullet prescribing either a full schema
  rewrite or replacement with a pointer to § 2.3.
- **MEDIUM / INV-12 test coverage gap on config-triple preservation**
  (B-M5) — the invariant prose contracted config-triple preservation
  in `mode:"near_misses"`, but the § 7 test description didn't
  assert it. Folded: the `test_mode_arm.cpp` description in § 7 now
  explicitly enumerates the four preserved fields
  (`auto_model_switch_enabled`, `floor_tier`, `min_dwell_sec`,
  `scope`) in both modes.
- **LOW / § 2.1 `return d` line cite at L31 inconsistent with the
  one-line-per-guard pattern** (B-H3 → LOW on verify) — folded:
  parenthetical now reads "16, 17, 18, 24, 25, 26, and 30–31 — the
  last guard's `if` wraps across two source lines".
- **LOW / `tests/features/README.md § "Bundles"` informal cite**
  (B-M2) — folded: § 7 now reads `§ "Bundles (ANTS-1217)"
  (literal section heading at tests/features/README.md:25)`.
- **LOW / `statsSlim` algorithm description didn't name
  `kStatsWindowMs`** (B-L1) — folded: the filter pseudocode now
  reads `nowMs - parseIso8601Ms(ts) <= kStatsWindowMs`.
- **LOW / subsystems.md `modelswitchledger` row line range off-by-
  one** (A-L1 / B-M1 / C-H1 — all three reviewers caught it) —
  spec cited L182–187; actual row is 7 lines spanning L182–188.
  Folded: § 8 subsystems.md bullet now reads "7-line depth at
  L182–188".
- **LOW / packaging verification was implicit** (C-L1) — folded:
  added a § 8 bullet stating packaging files were grepped and
  carry no auto-switcher mention.

Loop 3 reviewer dismissals:
- **A-M3** — claimed `claudestatuswidgets` row spans L134–143;
  actual span is L134–141 (verified by re-reading the file —
  L142 starts the next row). Spec's existing cite is correct;
  declined.
- **A-L2** — per-record byte range "250–350 B"; reviewer noted
  worst-case all-tokens record runs ~420 B. Typical records carry
  1–2 tokens; declined as informational (cap math still holds).
- **A-L3** — INV-9 grep alternation wording could mislead about
  production-code shape; INV-9 is clearly scoped to the test, and
  § 2.1 shows the production form (per-guard `QStringLiteral`).
  Declined.
- **B-M4** — explicit § 5.4 security-boundary re-statement for
  ANTS-1735 INV-9 (`tierArg` only via `tierName`). Declined: the
  new `recommendedTier` field is also enum-typed (`Tier`), so
  INV-9's contract holds without restatement; § 6 migration
  already says "additive Decision/Gate fields" which covers it.
- **C-M1** — CHANGELOG `### Changed` placement next to existing
  BREAKING entry. Editorial; let the implementation commit
  choose the ordering. Declined.
- **C-M3** — CLAUDE.md paragraph approaching readability limit;
  editorial. The C-M2 fold-in rescopes the gate sentence inside
  the existing paragraph, which is a small mitigation. Declined.
- **C-L3** — CHANGELOG section ordering convention. Editorial;
  declined.

### Loop 4 (2026-05-27) — folded

Three parallel reviewers, briefed identically with no prior-loop
fix list. Loop 4 returned no HIGH-severity findings (the spec is
implementation-ready against current source) plus a tighter MEDIUM
batch and a few LOW polish items.

- **HIGH / Header-relationship reciprocity for ANTS-1735 + ANTS-1890**
  (B-H1) — the spec declared "Composes with: ANTS-1735" but the
  reciprocal `Composed with:` line was missing from ANTS-1735's
  header (and similarly for ANTS-1890, whose `override_cooldown_active`
  token this spec records). The standard's § 3.2 *Optional
  relationship lines* convention says "use these so the dependency
  graph lives in the docs, not only in someone's head" — a one-way
  Compose is partial. Folded: § 8 ANTS-1735-autonomous-claude-code-model-switcher.md bullet now also
  prescribes appending a `**Composed with:** ANTS-1894` line;
  added a parallel § 8 bullet for ANTS-1890-scorer-v2-commit-intent-recency.md.
- **MEDIUM / Layman: line missing from header block** (B-M5) —
  per `docs/standards/specs.md` § 5.2, "where a spec describes
  user-facing behaviour, include a one-sentence plain-English
  Layman gloss." The `model_switch_stats` envelope IS the
  user-facing surface for the end-of-session summary. Folded:
  added a `**Layman:**` line under the header block matching the
  ROADMAP bullet's wording style.
- **MEDIUM / minDwellMs units ambiguity** (A-M1) — § 2.4(2) wording
  "set it from `autoCfg.value("min_dwell_sec")`" could be misread
  as "set it from the raw seconds value", which would silently
  truncate the dwell floor to 90 ms. Folded: the gate-construction
  bullet now explicitly notes the local is already in milliseconds
  via the existing `* 1000` on L1386.
- **MEDIUM / mcp-tools.md § 10 schema-hygiene partly addressed**
  (B-M3) — § 2.6(3) described the new `mode` enum addition but
  did not explicitly call out the three checklist points
  (additionalProperties:false preserved, NOT in required[], per-
  property description). Folded: the descriptor sub-bullet now
  enumerates (a) / (b) / (c) verbatim.
- **MEDIUM / INV-13 round-trip wording could mislead** (B-M1) —
  the spec phrased "Round-trip `toJson(fromJson(toJson(r)))` is
  identity for any well-formed Record"; the natural test direction
  `fromJson(toJson(r)) == r` is also true for any
  writer-produced Record (the writer always emits all keys), but
  NOT for hand-crafted legacy partial JSON (that's the back-compat
  path). Folded: INV-13 now distinguishes the two cases
  explicitly with the writer-canonical-form clarification, citing
  ANTS-1891 INV-10 as precedent.
- **MEDIUM / `bad_mode` cite anchored to line not section** (B-M2)
  — `mcp-error-codes.md:38` is fragile against row reordering.
  Folded: INV-12's cite now reads "under `docs/standards/mcp-error-codes.md`
  § \"Input validation\"" — category-anchored.
- **MEDIUM / test label not stated** (B-M4) — per
  `docs/standards/specs.md` § 3.6 the test section names the
  label. Folded: § 7 now adds a "Test label" paragraph stating all
  13 blocks inherit `features;fast` via their bundle.
- **MEDIUM / ROADMAP § 8(a) should default to pointer-form**
  (C-M3) — the verbatim schema rewrite invites future re-drift
  on any spec extension; the pointer form is rot-tolerant. Folded:
  § 8(a) "Record schema" sub-bullet now names the pointer form as
  the **preferred** fix.
- **LOW / `### Changed` CHANGELOG headline name-dropped C++ symbol**
  (C-L1) — `**ModelAutoSwitch::decide` now evaluates...**` leaks
  internal API. Folded: rewritten to user-facing frame —
  "**Auto-model switcher internals now report every reason a
  switch was held back.**"
- **LOW / Slim-block CHANGELOG entry leaked MCP arg surface**
  (C-L2) — the prose mentioned `scope:"project"` / `scope:"global"`
  in user-facing copy. Folded: dropped from the entry body.
- **LOW / `m_integration` type left implicit in § 2.5** (B-L3) —
  added an inline comment noting it's a `ClaudeIntegration *`
  member on the controller.

Loop 4 reviewer dismissals:
- **A-L1 / A-L3** — § 2.1 line cite phrasing for L30–31 wrap;
  Decision field ordering preference. Already addressed in loop 3
  or pure editorial.
- **B-L1 / B-L2** — ANTS-1217 spec-ID resolution inline; § 8 mixed
  registers. Editorial; declined.
- **C-M1** — CLAUDE.md paragraph readability cliff. The reviewer
  explicitly flagged it as an implementation-time judgement call;
  noted in the implementation PR description's TODO rather than
  the spec.
- **C-M2** — `subsystems.md` `modelswitchledger` row pre-existing
  drift relative to CLAUDE.md. Out of scope for ANTS-1894;
  logged as a separate small ROADMAP candidate ("subsystems.md
  modelswitchledger row catch-up to 1889/1891 surface") for a
  future debt sweep.
- **C-L3** — README auto-switcher mention at landing. Will be
  re-checked by the spec-acceptance commit's grep.

### Loop 5 (2026-05-27) — folded

Three parallel reviewers, identical-brief cold pass. Loop 5 surfaced
one real HIGH (the `Kind:` value violates the standard taxonomy) plus
fold-ins on CLAUDE.md anchoring durability, the ROADMAP correction's
"per-process" qualifier, and several smaller wording precisions. The
spec was hit by the cold-eyes skill's default `--max-loops=5` cap.

- **HIGH / `Kind: feature` violates `roadmap-format.md` § 3.5.3
  taxonomy** (B-H1) — verified at
  `docs/standards/roadmap-format.md:221` the recognised values are
  `implement` (new code for a planned feature), `fix`, `refactor`,
  …; `feature` is NOT in the list. ANTS-1735 / ANTS-1890 / ANTS-1891
  all use `Kind: implement.`. Folded: header changed to
  `**Kind:** implement.`. (Knock-on: ROADMAP bullet body still
  reads `Kind: feature.`; that's a separate authoring fix on the
  ROADMAP side, logged under § 8(a) as a third correction.)
- **HIGH / CLAUDE.md anchor will drift** (C-H1) — § 8 used absolute
  line cites (`CLAUDE.md L138–157`, `L156–157`); but the paragraph
  is mid-growth and the very edit we prescribe shifts the
  close-line by 2-3 lines, with the next sweep (ANTS-1895
  dry-run) shifting it again. Folded: CLAUDE.md bullet now
  anchors by **content** — quoting the paragraph's leading
  marker ("`model_switch_stats` (ANTS-1735, extended by ANTS-1889,
  sharpened by ANTS-1891)") and the target sentence verbatim ("Envelope
  readers should check `measured_downgrades > 0`...").
- **HIGH / ROADMAP rewrite dropped "per-process" qualifier** (C-H2)
  — the prescribed rewrite "emit-on-signature-change with a 5 s
  per-project floor" silently lost the persistence-semantic clue
  ("per-process" = controller-local, rebuilt on relaunch). Folded:
  the rewrite now reads "per-process, emit-on-signature-change
  with a 5 s per-project floor (rebuilt on relaunch — see § 2.4(4)
  for the controller-side maps)".
- **MEDIUM / ANTS-1895/1893 line shift larger than 1 line** (C-M2)
  — § 2.4(1) removes a ~4-line short-circuit block (the
  `kMinDwellMs` comment + the `if (...) return;` guard), not a
  single line. A "fix the cite" instruction risks a 1-line
  decrement when the actual shift is ~4. Folded: bullet now
  explicitly says "re-grep the actual post-edit line number for
  `focused->sendToPty(QStringLiteral("/model "))` and update BOTH
  bullet cites to the freshly-measured line"; also annotated each
  cited ROADMAP entry as ROADMAP-only inline.
- **MEDIUM / `configuredMinDwellMs` insert offset ambiguous** (A-M1)
  — § 2.2 said "alongside `msSinceLastSwitch`" which could land
  the field at L34 (dwell-family) OR L39 (after override-family
  field). Folded: inline comment now pins the slot between
  `msSinceLastSwitch` (L33) and `msSinceLastOverride` (L38) so it
  reads as a dwell-family field.
- **MEDIUM / `parseIso8601Ms` reuse not named** (A-M2) — the spec
  named `nowIso8601()` reuse on the write side but was silent on
  whether the stats / window-filter side reuses
  `parseIso8601Ms()`. Folded: § 2.3 now carries an explicit
  "Timestamp helpers" paragraph naming both helpers as reused.
- **MEDIUM / § 8 ANTS-1891 reciprocity acknowledgment** (B-M2) —
  bullet now states the reciprocity is verified bidirectional
  (header `Pairs with: ANTS-1894` already present in ANTS-1891-auto-switcher-trust-signal-v2.md),
  not just "no change needed".
- **MEDIUM / `bad_mode` parenthetical claimed invented convention**
  (B-M5) — the spec said line cite was "avoided per the standard's
  reorder-tolerance convention"; no such convention exists in any
  standards doc. Folded: parenthetical dropped; cite reads
  "`docs/standards/mcp-error-codes.md` § \"Input validation\""
  flat.

Loop 5 reviewer dismissals:
- **A-L1 / A-L2 / A-L3** — line cite L109–126 stable; `effectiveMinDwellMs`
  test path note; `claudeStateName` strings in test descriptions.
  All editorial or already addressed; declined.
- **B-M1** — sibling-spec reciprocity prescription is faithfully
  written; the actual sibling-edit happens at spec-acceptance
  commit time. Process risk, not spec defect; declined.
- **B-M3** — § 8 ANTS-1893/1895 bullet ROADMAP-only annotation.
  Folded above as part of C-M2.
- **C-M1** — CHANGELOG entry placement next to existing BREAKING
  entry. Editorial; punted to implementation commit.
- **C-M3** — `subsystems.md modelswitchledger` row pre-existing
  drift. Already logged as separate ROADMAP candidate in loop 4
  dismissals; declined.
- **C-M4** — CLAUDE.md "Behavioral notes" framing decision.
  Folded above as part of C-H1 (placement note added).
- **OQ-C1 / OQ-C2** — future-work observations (post-ANTS-1895
  CLAUDE.md trust-signal reader contract; AppStream metainfo
  release-time touch). Not actionable in this pass.

**Convergence note.** Loop counts of HIGH-severity verified
findings across the five loops: 10 → 8 → 0 → 1 → 3. The loop-4
zero was a local minimum; loop 5 surfaced one real taxonomy
finding plus two anchor-durability findings caused by mid-growth
documents. The cold-eyes skill's typical convergence range (1–3
loops) was exceeded; the spec is at the runaway-guard cap
(`--max-loops=5`) and the loop is paused at the user's gate per
the skill's protocol. Remaining open items (if a 6th loop runs)
will likely be small editorial polish on the loop log itself,
which has grown to ~470 lines.

### Amendment loop (2026-06-02) — ANTS-1946 / ANTS-1947

Re-opened to fold in two ROADMAP items: ANTS-1946 (new INV-14 —
`maybeEmitNearMiss` suppresses `target_equals_current` near-misses)
and ANTS-1947 (INV-10 `Σ by_blocked_by ≥ total` clarification).

**Loop 1 (2026-06-02) — folded.** Two cold lanes: A (spec-internal
consistency + test plan), B (spec vs cited code — `claudestatuswidgets.cpp`
`maybeEmitNearMiss`, `modelautoswitch.cpp` `decide`/`advanceStability`,
`modelnearmissledger.cpp`). Verified findings folded:

- **CRITICAL (A) / CRITICAL (B) — header / §10 audit-trail gap.** The
  amended header pointed to a §10 amendment-loop entry that did not
  yet exist, and the original header's "loop 6 returned polish-only"
  contradicted the convergence note (the original loop was *paused*
  at the cap, loop 6 never ran). Folded: header reworded to match
  §10; this Amendment-loop subsection added (written as the loop runs,
  not back-filled). Lane B's restatement (INV-14 reads as shipped while
  the guard is unimplemented) is by-design spec-first sequencing — the
  INV is the contract the implementer satisfies immediately after the
  spec is clean; INV present-tense is standard spec voice. No tense
  downgrade made.
- **HIGH (A) — `statsFull` example carried `target_equals_current: 9`**
  in `by_blocked_by`, which INV-14 makes impossible (those records are
  never emitted). Folded: example reworked to
  `composer_not_empty:31 / dwell_time_insufficient:14 /
  override_cooldown_active:3`.
- **HIGH (A) — §7 `test_core` bundle mislabelled "all 10"** for an
  11-item list (10 ledger/decide + 1 MCP), contradicting §4's "11
  total". Folded: relabelled "all 11 (10 pure ledger/decide + 1 MCP)".
- **MEDIUM (A) — INV-10 example summed to exactly `total`** (42),
  failing to witness the new strict `Σ > total` clarification. Folded
  with the HIGH-A example rework: counts now sum to 48 > 42, with an
  explicit note tying the example to INV-10.

Lane B confirmed (INFO, no change needed) that INV-14's biconditional
(`target_equals_current` ⟺ `clampToFloor(g.recommended,g.floor) ==
g.current`), the "two tokens always co-fire" claim (`advanceStability`
increments `ticksAtCurrent`, not `ticksStable`, when `target==current`),
and the INV-10 `tallyBlockedBy` one-per-distinct-token semantics all
match the code as written.

**Loop 2 (2026-06-02) — clean.** Loop 1 fixes folded above; the amended
spec was verified by a self-check at fold time (all four High/Medium
findings addressed consistently). Amendment loop closed.
