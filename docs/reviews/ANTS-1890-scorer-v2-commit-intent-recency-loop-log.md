# ANTS-1890 — Scorer v2: commit-intent + recency weighting + override cool-down — review loop log

Record for [`../specs/ANTS-1890-scorer-v2-commit-intent-recency.md`](../specs/ANTS-1890-scorer-v2-commit-intent-recency.md) § Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-05-26) — 3 independent reviewers (correctness/grounding, consistency, testability/product-fit)

Convergent findings, all verified against current source and folded:

- **CRITICAL** — proposed `-3` delta did not actually reach `Tier::Haiku`
  from a fully-positive window (`sc = +6 − 3 = +3 → Opus`), exactly
  failing the motivating "commit after heavy refactor" case. → §2.1
  switched to a **hard override** (`hasCommitIntent → return
  Tier::Haiku directly`), bypassing the additive ladder. INV-2
  reworded.
- **HIGH** — line citation `claudestatuswidgets.cpp:1217` was wrong
  (line is the recommender chip body); actuator is at L1311, fill-in
  at L1503. → §2.4 fixed; all `modelrecommender.cpp` line ranges
  rechecked against current source.
- **HIGH** — recency weighting × legacy threshold contradiction
  (INV-3 changes mass, INV-4 promised byte-equivalent preservation).
  → §2.2 narrowed weighting to count-based features only
  (`fileWriteCount`, `avgLen`); `toolDiversity` and `planKeyword`
  stay unweighted; legacy "byte-equivalent" claim retired and INV-4
  reworded around behavioural cases on weighted writes.
- **HIGH** — `release` and `merge` keywords had too broad a false-
  positive surface ("release plan", "merge approaches"). → §2.1 list
  trimmed to `commit/push/stage/bump/rebase`; OQ-1 documents re-tune.
- **HIGH** — `hasCommitIntent` word-boundary regex underspecified
  for inflected forms. → §2.1 spells the per-keyword pattern
  `(?:^|\W)/?<kw>(?:ed|ing|s)?(?:\W|$)`; INV-1 enumerates inflected
  forms.
- **HIGH** — override cool-down was a single global scalar, ignoring
  the ledger's per-project records. → §2.4 switched to
  `QHash<QString,qint64> m_lastOverrideMsByProject`; new INV-8.
- **HIGH** — cached scalar had a restart hole (`m_lastOverrideMs = 0`
  after launch missed any in-process-recorded override). → §2.4 added
  a bootstrap ledger read at controller construction; new INV-7
  covers it.
- **MEDIUM** — sentinel `0 = no override` collided with a reachable
  Δ=0 same-millisecond value. → §2.4 switched to `-1` sentinel;
  INV-6 gate updated.
- **MEDIUM** — RAM estimate "~32 KB single user turn" was a guess.
  → §4 dropped to "bounded by the 512 KB tail-read budget".
- **MEDIUM** — proposed `tests/features/scorer_v2/` bundle would
  violate the project's existing-bundle convention. → §6 absorbs
  the new tests into existing `model_recommender/`,
  `model_auto_switch/`, and `model_auto_switch_outcome_fillin/`
  bundles.
- **MEDIUM** — `weightForTurnIndex(total==0)` was undefined.
  → §2.2 comment + INV-3 wording state the caller short-circuits
  on empty windows.
- **MEDIUM** — manual recipe assumed too much. → §6 recipe now states
  preconditions explicitly with concrete verification commands.
- **LOW** — OQ-1, OQ-2 were already answered. → both moved to §5
  "Out of scope" with their verdicts inline.
- **LOW** — Layman line was too long / jargon-heavy. → §7 trimmed to
  two sentences.
- **LOW** — `modelautoswitch.cpp` cited without path:line. → §1
  uses `decide()` + `kMinDwellMs = 90'000` symbol citations.
- **LOW** — `ANTS-1735-autonomous-claude-code-model-switcher.md §5` cross-link was not bidirectional. → §7
  states it explicitly; the impl commit will amend ANTS-1735-autonomous-claude-code-model-switcher.md.
- **LOW** — ROADMAP `Lanes:` lists `modelswitchledger` but the spec
  doesn't modify it. → §7 lists the lane removal as a cross-doc
  follow-up.
- **INFO** — Loop 2 will brief reviewers cold against the rewritten
  spec, with no fix list — per CLAUDE.md rule 14.

### Loop 2 (2026-05-26) — 2 fresh reviewers (correctness/grounding, consistency/cross-doc), no fix-briefing

Convergent findings, all verified and folded:

- **HIGH (correctness)** — line citations drifted past loop 1's fixes:
  `fileWriteCount >= 4` is at L129 (was cited as L128); mechanical
  feature is at L133-134 (was L132-133); ladder body is L126-147 (was
  L127-148); `kPlanKeywords` is L15-22 (was L14-22). → all corrected
  against current source.
- **HIGH (consistency)** — ROADMAP.md ANTS-1890 entry title said
  "dominant-intent" but the spec body says "commit-intent". → ROADMAP
  bullet title aligned to "commit-intent".
- **HIGH (consistency)** — ROADMAP bullet body still described the
  pre-loop-1 `-3` delta semantics. → ROADMAP body rewritten to
  describe the hard-override semantics.
- **HIGH (consistency)** — ROADMAP `Lanes:` still included
  `modelswitchledger` despite §2.5 dropping it. → lane removed from
  the ROADMAP bullet in the same edit batch (no longer a follow-up).
- **HIGH (consistency)** — ANTS-1735-autonomous-claude-code-model-switcher.md §5 "Heuristic breadth" line
  did not cross-link ANTS-1890. → amended in this loop's batch (no
  longer "in the impl commit").
- **MEDIUM (consistency)** — `docs/subsystems.md` modelrecommender entry
  did not mention `hasCommitIntent` / `weightForTurnIndex`. → entry
  amended in the same batch (new symbols other code may grep for
  should be on the index now, not at impl time).
- **MEDIUM (consistency)** — Status line did not record the loop-1
  fold per docs/standards/specs.md §5.7. → updated to "spec draft,
  cold-eyes loops 1 + 2 folded (2026-05-26)".
- **MEDIUM (consistency)** — §5 OQ-1 / OQ-2 prefix collided with
  ANTS-1735's "real open questions" prefix. → renamed to §5.1 / §5.2
  with an explicit note that these are re-tune triggers, not open
  design questions.
- **MEDIUM (correctness)** — `m_lastOverrideMsByProject` comment
  read "Keyed by focused-tab project root canonical path" — accurate
  but misleading (the cache is keyed by the ledger record's `project`
  field; reads in `refreshAutoModelSwitch` then look up by the
  currently-focused tab's `shellCwd()` and the keys align because
  records are written that way at switch time). → comment expanded
  with the `claudestatuswidgets.cpp:1413` cross-reference.
- **MEDIUM (correctness)** — bootstrap pattern was sketched in prose
  but not shown. → §2.4 now carries the explicit `for (const Record
  &r : readRecords(…)) { ... slot = max(slot, parseIso8601Ms(r.ts));
  }` block; the `parseIso8601Ms returns 0 on failure → silent skip`
  invariant is noted inline.
- **MEDIUM (correctness)** — INV-5 source-grep was anchored on a
  fragile `QFile f(` count that would mis-fire because
  `thinkingLevelFromLatestUserTurn` and others in the same TU also
  declare `QFile`. → INV-5 reworded to anchor on `f.open(QIODevice::`
  *between* the `Result score(` opener and its matching closing brace
  (function-body-scoped grep), with the home bundle named explicitly.
- **LOW (consistency)** — Source-line prefix used `user-...` instead
  of the canonical `user-request-...` form. → fixed.
- **LOW (correctness)** — `def.currentModel` is captured at L83-86
  *before* the proposed override branch could run; the spec did not
  pin the code-order constraint. → §2.1 now states explicitly:
  "the override branch must run *after* `def.currentModel` is
  captured … and *after* the assistant-turn reverse walk".
- **LOW (correctness)** — `clampToFloor` signature + Tier order were
  asserted but not cited. → §2.1 now cites
  `modelautoswitch.cpp:6-12` (returns floor when rec < floor) and
  `modelrecommender.h:8` (`Haiku=0 < Sonnet=1 < Opus=2`).
- **LOW (consistency)** — INV-8 was hosted in the fill-in bundle but
  is structurally a gate-behaviour invariant. → moved to
  `tests/features/model_auto_switch/` next to INV-6 (where the
  per-project interpretation lives).
- **LOW (consistency)** — array/bare-string handler cited as L178-200
  but the actual fork is at L206-213. → corrected.
- **INFO** — Loop 3 will brief reviewers cold against the
  twice-rewritten spec with no fix list; per CLAUDE.md rule 14 the
  loop runs until a clean pass returns zero verified findings.

### Loop 3 (2026-05-26) — 2 fresh reviewers (grounding/cross-doc, testability/implementability), no fix-briefing

Convergent findings, all verified and folded:

- **CRITICAL (testability)** — INV-2 pre-fix RED state was
  arithmetically wrong against `modelrecommender.cpp:138`: 4+ writes
  alone yields `sc=+2` (Sonnet), not Opus (which requires `sc>=3`).
  → INV-2 rewritten with a discriminating fixture: 20 turns each
  containing one `Edit` tool_use AND one assistant text-block with a
  plan-keyword (e.g. "refactor") → `many_writes(+2) +
  plan_keyword(+2) = sc=+4 → Opus`. Post-fix → Haiku.
- **HIGH (correctness)** — empty-window short-circuit at L79 returned
  before `def.currentModel` was captured; the prior wording "override
  branch runs *after* the reverse walk" would also fail to fire on a
  fresh project where the user types "commit and push" before
  Claude responds. → §2.1 added an explicit **Function-body
  ordering** subsection: walk → override-branch (fires even on empty
  assistant-turn windows) → empty-window short-circuit → ladder.
- **HIGH (correctness)** — §2.2 overclaimed "1:1 mass calibration"
  for the doubled threshold; the calibration only holds under
  uniform write distribution. → §2.2 trimmed the claim and pointed
  at INV-4's numeric examples.
- **HIGH (testability)** — INV-7 promised a "behavioural fixture"
  but provided no injection seam for `defaultLedgerPath()`. → §2.4
  extracts a new private helper
  `ClaudeStatusBarController::seedOverrideCacheFromLedger(const
  QString &ledgerPath)` (named, separately testable); INV-7 names
  it as the test injection point. Bootstrap call site is pinned to
  `attach()` (after services wired, before the 2 s timer's first
  tick).
- **HIGH (testability)** — INV-4 lacked numeric fixtures. → INV-4
  now lists the exact arithmetic for the recent-cluster (sum≈11.368)
  and old-cluster (sum≈4.632) cases, with index sets named.
- **MEDIUM (consistency)** — §1.1 prose said "single Bash-only turn
  cannot move the average" — there is no average of writes;
  `fileWriteCount` is a sum. → reworded to "never reduces the prior
  19 turns' write count".
- **MEDIUM (testability)** — INV-2's `clampToFloor` test home was
  ambiguous (lives in `ModelAutoSwitch::decide`, not in `score()`).
  → split into INV-2 (scorer returns Haiku, in
  `model_recommender/`) + INV-2a (`CommitIntentTargetClampedByGate`,
  in `model_auto_switch/`).
- **MEDIUM (implementability)** — §2.1 was ambiguous on where
  `latestUserText` is collected vs the override branch. → §2.1's
  new Function-body ordering subsection lays out steps 1–5
  explicitly.
- **MEDIUM (testability)** — `hasCommitIntent` case-folding was
  unspecified. → §2.1 doc-comment states the caller pre-lowercases
  with `QString::toLower()` mirroring
  `thinkingLevelFromLatestUserTurn` at `modelrecommender.cpp:217`.
- **LOW (consistency)** — ROADMAP bullet did not name the
  per-project cool-down scope explicitly. → covered by the §2.4
  body of the spec; ROADMAP bullet kept tight (per-project is a
  spec invariant, not a headline detail).
- **LOW (correctness)** — `parseIso8601Ms`-returns-0 + operator[]
  auto-insert interaction was correct but not documented. → the
  fill-in code block in §2.4 carries the explanatory comment
  (zero-slot via operator[] degrades to -1 sentinel via the read
  path's `(lastMs > 0)` ternary, i.e. "no override on record").
- **LOW (testability)** — INV-5 grep-tool ambiguity. → INV-5 names
  the home bundle (`tests/features/model_recommender/`) and the
  anchored grep (`f.open(QIODevice::` count between `Result
  score(` and the matching closing brace); an implementer picks
  the tool (awk, sed, regex) without ambiguity about WHAT to grep
  for.
- **INFO** — Loop 3 reviewers verified the loop-2 cross-doc fixes
  held: ANTS-1735-autonomous-claude-code-model-switcher.md §5 carries the ANTS-1890 cross-link;
  `docs/subsystems.md` modelrecommender entry mentions
  `hasCommitIntent` + `weightForTurnIndex`; ROADMAP title says
  "commit-intent" and bullet describes hard override.
- **INFO** — Loop 4 will brief reviewers cold against the
  thrice-rewritten spec; per CLAUDE.md rule 14 the loop runs until
  a clean pass returns zero verified findings.

### Loop 4 (2026-05-26) — 2 fresh reviewers (correctness/cross-doc, testability/product-fit), no fix-briefing

Reviewer 1 returned **CLEAN PASS** — all numeric claims (sum-of-weights=40, recent-cluster=11.368, old-cluster=4.632) and every cited source location (modelrecommender.cpp L8/15-22/34/65-77/79/83-86/126-147/129/133-134/138/206-213/217; modelautoswitch.cpp:6-12; modelrecommender.h:8; claudestatuswidgets.cpp:1311/1413/1503; modelswitchledger parseIso8601Ms behavior) verified against current source. Cross-doc consistency held (ROADMAP title + lanes + body description, ANTS-1735 §5 cross-link, docs/subsystems.md mentions).

Reviewer 2 returned 1 MEDIUM + 4 LOW, all folded:

- **MEDIUM (testability)** — INV-2 fixture lacked a user-turn helper
  convention; two implementers would diverge on signature. → §6
  now points at the existing `appendUserTurn` pattern in
  `tests/features/model_recommender/test_model_recommender.cpp:179+`
  (the ANTS-1888 thinking-level tests) and prescribes either
  extending `writeSyntheticTranscript` or adding a sibling
  `writeSyntheticTranscriptWithUserTail`. Discriminating fixture
  text (each turn = 1 Edit + plan-keyword like "refactor") spelled
  out so sc=+4 → Opus pre-fix.
- **LOW (consistency)** — Status line L3 said "loops 1+2 folded"
  but the log carried a full Loop 3 section. → bumped to "loops
  1 + 2 + 3 folded".
- **LOW (product-fit)** — `bump`'s doc-edit shape ("bump versions
  in README") not surfaced as a regret-risk in §5.1. → §5.1 now
  names both false-positive shapes and a narrowing path if the
  doc-bump pattern dominates regrets.
- **LOW (product-fit)** — 10-min cool-down could double-bite on
  rapid `/model` toggles (user re-choosing manually). → §5.2 now
  documents that the ledger does not distinguish "override of
  auto" from "user re-choosing" and that the re-tune trigger
  should not over-fit on this shape.
- **LOW (implementability)** — `seedOverrideCacheFromLedger`
  marked as `// New private helper` but the behavioural test
  injects via direct call. → comment bumped to `// New public
  helper` matching the controller's existing public refresh/fill
  surface.
- **INFO** — Loop 5 will brief reviewers cold against the
  four-times-rewritten spec to verify convergence; per CLAUDE.md
  rule 14 the loop terminates only on a pass returning zero
  verified findings.

### Loop 6 (2026-05-26) — TDD RED discovery — fold

Implementation phase caught a spec-internal contradiction that 5 cold-eyes
reviewers missed:

- **HIGH (correctness)** — INV-1's flat `(?:ed|ing|s)?` suffix regex can't
  match the spec's OWN enumerated example `"I just committed; now push it"`
  (`committed` = `commit`+`ted`, requires double-t consonant gemination
  handling), nor common inflections `pushes`/`rebased`/`staged` (English
  morphology: -es plural for -sh endings, final-e elision for -ase/-age
  verbs). Discovered during TDD RED: 1 of 6 unit tests
  (`CommitIntentInflectedFormsMatch`) failed when the spec's regex was
  implemented verbatim. → §2.1 rewritten with per-keyword alternations
  (`commit(?:s|ted|ting)?` / `push(?:es|ed|ing)?` /
  `stage(?:s|d|ing)?` / `bump(?:s|ed|ing)?` / `rebase(?:s|d|ing)?`),
  each compiled as `(?:^|\W)/?<alt>(?:\W|$)`. Implementation passes all
  6 tests against the corrected pattern. The fix preserves INV-1's
  substring-rejection semantics (`committee`/`pushcart`/`thinkpad` /
  `bumper` all stay rejected by the word boundaries) — confirmed by
  `CommitIntentSubstringRejected`.
- **INFO** — Lesson for the cold-eyes methodology: regex specs should be
  checked against their own enumerated examples by hand (or by sample
  fixture grep) during the loop, not assumed correct just because the
  pattern looks plausible. Cold-eyes reviewers verify *citations to
  existing source* faithfully; verifying *the spec's own future code*
  needs the implementation-loop discovery this round provided.

### Loop 5 (2026-05-26) — 1 fresh reviewer (convergence check), no fix-briefing — CLEAN PASS

Reviewer independently verified, line-by-line:

1. Every cited `file:line` in the spec resolves to the claimed
   content in current source (modelrecommender.cpp L8 / L15-22 /
   L34 / L65-77 / L79 / L83-86 / L126-147 / L129 / L133-134 / L138
   / L206-213 / L217; modelautoswitch.cpp:6-12; modelrecommender.h:8;
   claudestatuswidgets.cpp:1311 / :1413 / :1503;
   modelswitchledger parseIso8601Ms behavior at L420-423).
2. All numeric examples confirmed by direct computation: sum of
   weights across 20 turns = 40.0; recent-cluster {16,17,18,19}
   weighted sum = 11.368; old-cluster {0,1,2,3} weighted sum =
   4.632.
3. INV-2 pre-fix RED state arithmetically traced: 20 turns ×
   {Edit + plan-keyword text "refactor the cache"} yields
   fileWriteCount=20 (many_writes +2), planKeyword=true (+2),
   toolDiversity=1 (no add), avgLen≈18 (no add), mechanical
   inapplicable → sc=+4 → Tier::Opus at modelrecommender.cpp:138.
   Discriminating against post-fix Tier::Haiku.
4. `readRecords(const QString &path)` signature in
   modelswitchledger.h:56 matches the §2.4 bootstrap call.
5. Function-body ordering (walk → override → empty-check → ladder)
   feasible without breaking the existing 20-turn cap (L69 guard)
   or the tail-read seek (L49-52).
6. `appendUserTurn` helper exists at
   tests/features/model_recommender/test_model_recommender.cpp:157
   (used from L179+ in the ANTS-1888 thinking-level tests).
7. ROADMAP title "commit-intent" matches spec H1; body describes
   hard override; `Lanes:` drops `modelswitchledger`.
8. ANTS-1735-autonomous-claude-code-model-switcher.md §5 cross-link to ANTS-1890 present.
9. docs/subsystems.md modelrecommender entry mentions both
   `hasCommitIntent` and `weightForTurnIndex`.
10. Spec conforms to docs/standards/specs.md §3 (H1; Status/Kind/
    Source header; §1 Problem; §2 Surface with subsections; §3
    Invariants in `- **INV-N** — body. *Test:* …` form; §4 RAM;
    §5 Out-of-scope; §6 Tests; §7 Cross-doc; Cold-eyes loop log).

**No verified findings of CRITICAL / HIGH / MEDIUM / LOW
severity.** Spec is signed-off-ready pending user review.
