# ANTS-1890 — Scorer v2: commit-intent + recency weighting + override cool-down

**Status:** spec draft, cold-eyes loops 1 + 2 + 3 + 4 + 5 folded, loop-6 TDD-discovered §2.1 morphology fix folded (2026-05-26) — implementation in progress.
**Kind:** implement.
**Source:** ROADMAP.md ANTS-1890 (user-request-2026-05-26 — "Haiku can
probably handle commit and push events").

**Composes with:** ANTS-1735 §5 (this spec is the heuristic-breadth work
that §5 deferred). **Extends:** ANTS-1226 (the original passive
recommender — no separate spec file; the contract is the scorer source
+ `tests/features/model_recommender/spec.md`).
**Composed with:** ANTS-1894 (near-miss telemetry records
`override_cooldown_active` ticks via the canonical 7-token taxonomy —
ANTS-1890's INV-7/INV-8 contract is observed unchanged).

## 1. Problem

`ModelRecommender::score` (`src/modelrecommender.cpp` `score()` at L34)
looks at the **last 20 assistant turns** with **equal weight per turn**
and never reads the user's most recent prompt. Three real-world failure
modes follow.

1. **Mechanical-tail leak.** After an Opus-heavy refactoring session,
   the user types "commit and push". The recommender keeps recommending
   Opus/Sonnet because the 20-turn window is still dominated by the
   prior session's writes (`fileWriteCount >= 4` at `modelrecommender.cpp:129`).
   The single Bash-only turn the new prompt produces never reduces
   the prior 19 turns' write count. The `mechanical` feature
   (`modelrecommender.cpp:133-134`) only fires when the **whole window**
   lacks writes — never when it's the immediate tail.
2. **No intent signal in the latest user prompt.** The scorer reads
   only assistant turns (the JSONL walk at `modelrecommender.cpp:65-77`
   filters `type=="assistant"`). The user's *typed intent*
   ("commit", "push", "bump", "rebase") is the strongest signal for
   "this turn is mechanical" and the scorer ignores it completely.
   (Plan-keyword detection exists for the *opposite* direction at
   `modelrecommender.cpp:15-22` but only inside assistant text.)
3. **No override cool-down.** The ANTS-1735 ledger records
   `userOverrideWithin5` per INV-11 when the user types `/model X`
   shortly after an auto-switch (`modelswitchledger.cpp` —
   `computeOutcome`). `ModelAutoSwitch::decide`
   (`src/modelautoswitch.cpp` — `decide()`; `kMinDwellMs = 90'000`)
   re-attempts the next auto-switch as soon as `kMinDwellMs` elapses
   — even when the previous attempt was just overridden. The user's
   disagreement should suppress further auto-switches for longer than
   the dwell.

Together these gaps are the largest remaining source of regret /
under-route that gates the §8 OQ-3 default-ON flip (the
`regret_rate + under_route_rate < 15%` threshold in
`docs/specs/ANTS-1735-autonomous-claude-code-model-switcher.md:493-498`). The user opted in 2026-05-26 —
every regret-flag in the ledger over the coming weeks is feedback to
fix here.

## 2. Surface

### 2.1 Commit-intent — hard override on the latest user turn

```cpp
// src/modelrecommender.cpp — new pure helpers, no I/O.
// The 5 commit-intent verbs each compile to a per-keyword alternation
// pattern that handles its actual English morphology — see comment block
// in the helper for the table. A flat `(?:ed|ing|s)?` suffix (loop-5
// draft) cannot match `committed` (double-t gemination), `pushes`
// (-es plural for -sh ending), or `rebased`/`staged` (final-e elision)
// — caught at TDD RED, see Loop-6 fold below.
//
// Narrow, conservative verb set — `release` and `merge` are EXCLUDED
// from the v1 list because of well-known false-positive shapes
// ("release plan" / "merge these two approaches"); §5.1 documents the
// re-tune path.
const QStringList kCommitIntentAlternations{
    QStringLiteral("commit(?:s|ted|ting)?"),
    QStringLiteral("push(?:es|ed|ing)?"),
    QStringLiteral("stage(?:s|d|ing)?"),
    QStringLiteral("bump(?:s|ed|ing)?"),
    QStringLiteral("rebase(?:s|d|ing)?"),
};

// Each alternation compiled as `(?:^|\W)/?<alt>(?:\W|$)`: word-boundary
// stem, optional `/` prefix for the `/commit` slash-command form.
// Lazy-built once per process. Linear; no nested quantifiers — no ReDoS
// surface. Slash-prefix tolerated for parity with the recommender chip's
// /model click. Caller lowercases `latestUserText` via
// `QString::toLower()` BEFORE invoking, mirroring
// `thinkingLevelFromLatestUserTurn` at `modelrecommender.cpp:217`
// (regex is then plain ASCII; no need for `CaseInsensitiveOption`).
bool hasCommitIntent(const QString &latestUserText);
```

**Hard override semantics.** When `hasCommitIntent(latestUserText)`
returns true, `score()` returns `Result{tier=Haiku, reason="commit_intent",
currentModel=<from transcript or empty>}` directly — skipping the
additive ladder entirely. The Haiku target is then subject to the
autoswitcher's existing `clampToFloor` gate in
`ModelAutoSwitch::decide` (verified at `modelautoswitch.cpp:6-12` —
returns `floor` when `rec < floor`; Tier enum order is
`Haiku=0 < Sonnet=1 < Opus=2` per `modelrecommender.h:8`), so a user
on `floor:"sonnet"` still gets Sonnet. A `-3` *delta* was considered
and rejected: from a fully-positive window (`many_writes:+2 +
plan_keyword:+2 + tool_diversity:+1 + long_prompts:+1 = +6`), `-3`
still lands at `sc=+3 → Opus`, which exactly *fails* the motivating
"commit after a heavy refactor" case. A hard override has no such
failure mode.

**Function-body ordering.** The reworked `score()` runs in this fixed
order (the override branch slots between the walk and the additive
ladder so the reverse-walk runs once for BOTH the assistant-turn
sequence and the `latestUserText` collection — §2.3):

1. Tail-read the transcript (existing — `modelrecommender.cpp:36-58`).
2. Reverse-walk lines, collecting (in one pass) both the last 20
   `{type:"assistant"}` turns and the latest 1 `{type:"user"}` line
   (§2.3). `def.currentModel` is set inside this walk from the last
   assistant turn (current `modelrecommender.cpp:83-86`).
3. **Override branch (new).** If `hasCommitIntent(latestUserText)`
   returns true → return `Result{Haiku, "commit_intent",
   def.currentModel}`. `def.currentModel` may be empty when no
   assistant turns precede the user prompt (a fresh project's first
   prompt); this is acceptable — the autoswitcher's downstream
   `clampToFloor` decision does not consume `currentModel`, only the
   `tier` field. The recommender chip's no-op shape (when the
   recommendation matches the current tier) just shows nothing,
   which is fine on a fresh session.
4. If `turns.isEmpty()` → return `def` (existing
   `modelrecommender.cpp:79`; moved after the override branch so a
   fresh-session commit-intent still fires).
5. Feature extraction + additive ladder (existing, with the
   recency-weighting modifications of §2.2).

### 2.2 Recency weighting — applied only to count-based features

The current additive ladder at `modelrecommender.cpp:126-147` uses two
count-based features that can compose cleanly with weighting:

- `fileWriteCount` — incremented per `Edit`/`Write` tool_use across
  the 20-turn window.
- (`avgLen` is already an average; weighting is folded in as a
  weighted average — see below.)

`toolDiversity` (a set cardinality) and `planKeyword` (a window-wide
boolean) do NOT compose with per-turn weighting and stay equal-weight
in v2.

```cpp
// idx is 0-based, ascending in time (idx=total-1 is the LATEST turn).
// Returns w in [1.0, 3.0]. Linear: w = 1.0 + 2.0 * (idx / max(1, total-1)).
// Undefined for total<1; caller must short-circuit on an empty window
// (score() at modelrecommender.cpp:79 already returns def on no turns).
double weightForTurnIndex(int idx, int total);
```

Concrete substitutions in `score()`:

- `fileWriteCount` accumulator becomes a weighted sum
  `weightedWrites += weightForTurnIndex(idx,total) * writesInTurn`.
  Threshold raises from `>= 4` to `>= 8` (the sum of weights across a
  full 20-turn window is `sum_{idx=0..19} (1 + 2*idx/19) = 40`, twice
  the unweighted sum 20 of equal weights). The 1:1 mass calibration
  holds **only under a uniform write distribution**; recent clustering
  trips earlier and old clustering trips later, by design (see the
  numeric examples in INV-4).
- `avgLen` (total length over msgCount) becomes the same ratio computed
  against weighted-total / weighted-count. The `>= 500` threshold is
  unchanged because both numerator and denominator scale together.
- `toolDiversity` and `planKeyword` stay byte-identical (set cardinality
  and OR across all turns; weighting is meaningless for them).

**Honest threshold note.** Introducing weighting *will* change boundary
behavior at the recommendation level — a session whose writes cluster
in the most recent 5 turns will trip `many_writes` where v1 wouldn't,
and vice versa. This is the *intent* of the change, not a regression.
No INV-4 "byte-equivalent v1 preservation" claim is made; see §3.

### 2.3 Latest-user-turn collection

The existing JSONL walk at `modelrecommender.cpp:65-77` is reverse-order
through ≤512 KB of tail-read lines, filtered to `type=="assistant"`.
Add a single-pass companion that collects the LAST `{type:"user"}`
line in the same loop (no second tail-read, no second `QFile::open`).
Joined `text` content blocks (handling the array-or-bare-string
shapes that `thinkingLevelFromLatestUserTurn` already handles per
ANTS-1888 — see `modelrecommender.cpp:206-213` for the
`content.isArray()` / `content.isString()` fork) become
`latestUserText` and feed `hasCommitIntent`.

### 2.4 Override cool-down — per-project, restart-safe

```cpp
// src/modelautoswitch.h — Gate gains one field, decide() gains one rule.
struct Gate {
    // ... existing fields ...
    // -1 = no override on record OR no ledger seen yet (cool-down does
    //      not block); >= 0 = ms since the most recent
    //      project-scoped override.
    qint64 msSinceLastOverride = -1;
};

// decide() refuses to act when (msSinceLastOverride >= 0 &&
// msSinceLastOverride < kOverrideCooldownMs). The existing dwell rule
// (msSinceLastSwitch >= kMinDwellMs) stays — the cool-down is a
// separate, stricter floor that applies only when a project-scoped
// override is on record. Sentinel -1 avoids the "same-millisecond
// race = freshly-recorded override is missed" hole that 0 would
// introduce.
constexpr qint64 kOverrideCooldownMs = 10 * 60 * 1000;   // 10 min
```

**Cool-down scope: per-project.** Records carry a `project` field
(`modelswitchledger.h:39`). An override in project A must NOT block an
auto-switch in project B. The controller holds:

```cpp
// src/claudestatuswidgets.h — controller member.
// Keyed by the ledger record's `project` field. Records are written
// at switch time from the focused tab's shellCwd() (see
// `claudestatuswidgets.cpp:1413` — `rec_.project = focused->shellCwd();`),
// so reads in refreshAutoModelSwitch() look up by the currently
// focused tab's shellCwd() and the keys align.
QHash<QString, qint64> m_lastOverrideMsByProject;
```

**Update flow.** `ClaudeStatusBarController::fillPendingLedgerOutcomes`
(`claudestatuswidgets.cpp:1503`, throttled to ≤ once / 30 s) already
loads the ledger and runs `computeOutcome` per pending record. After
each call:

```cpp
if (computed.userOverrideWithin5 && !computed.pending) {
    const qint64 tsMs = ModelSwitchLedger::parseIso8601Ms(rec.ts);
    // parseIso8601Ms returns 0 on parse failure; the (tsMs > slot)
    // guard skips bad records (0 > 0 is false). operator[] auto-
    // inserts a default-zero slot for an unknown project key — fine
    // because the read path's `(lastMs > 0)` ternary translates a
    // stuck-at-zero slot back to the -1 sentinel, so an unparseable
    // record degrades to "no override on record" (cool-down does
    // not block — acceptable; parse failures are vanishingly rare
    // since we control the writer).
    auto &slot = m_lastOverrideMsByProject[rec.project];
    if (tsMs > slot) slot = tsMs;
}
```

**Bootstrap (restart safety).** The cached scalar is a perf
optimisation; the ledger is authoritative. On controller construction:
do **one** ledger read (cheap — small file, ≤256 KiB cap per ANTS-1735
INV-10) to seed `m_lastOverrideMsByProject` from any settled records
already on disk. Without this, an override 30 s before app restart
would be "forgotten" — the very turn that should fire cool-down
passes — because `fillPendingLedgerOutcomes` only walks PENDING
records, and a 30-s-old override is usually already settled.

```cpp
// New public helper on the controller — extracted so behavioural
// tests in tests/features/model_auto_switch_outcome_fillin/ can
// inject a fixture ledger path (defaultLedgerPath() reads from
// QStandardPaths::GenericCacheLocation, which is process-global and
// unsuitable for behavioural tests). Public visibility matches the
// existing pattern for the controller's refresh/fill methods
// (refreshTasksButton, refreshModelChip, fillPendingLedgerOutcomes
// — all public in claudestatuswidgets.h). Called once from
// attach() after services are wired but before the 2 s status
// timer fires.
void ClaudeStatusBarController::seedOverrideCacheFromLedger(
    const QString &ledgerPath)
{
    for (const ModelSwitchLedger::Record &r
            : ModelSwitchLedger::readRecords(ledgerPath)) {
        if (!r.outcome.userOverrideWithin5 || r.outcome.pending) continue;
        const qint64 tsMs = ModelSwitchLedger::parseIso8601Ms(r.ts);
        auto &slot = m_lastOverrideMsByProject[r.project];
        if (tsMs > slot) slot = tsMs;
    }
}
```

Caller (in `attach()`): `seedOverrideCacheFromLedger(
ModelSwitchLedger::defaultLedgerPath());`

`ClaudeStatusBarController::refreshAutoModelSwitch`
(`claudestatuswidgets.cpp:1311`, the 2 s status-tick auto-switch
actuator) reads from the cache and populates the Gate:

```cpp
const qint64 lastMs = m_lastOverrideMsByProject.value(projectRoot, -1);
gate.msSinceLastOverride = (lastMs > 0) ? (now - lastMs) : -1;
```

### 2.5 Ledger record annotation (reuse, don't extend)

Existing `Record::scoreReason` is set from `ModelRecommender::Result::reason`
at the actuator. The new `commit_intent` token surfaces there
automatically — no schema change, no migration. (Spec §2.5 of ANTS-1735
already documents the unstructured-string convention.) The ROADMAP
bullet's `Lanes:` is amended to drop `modelswitchledger` (we read its
records but do not change its file format or API).

## 3. Invariants

- **INV-1** — `hasCommitIntent("commit and push")`,
  `hasCommitIntent("git commit -m 'x'")`, `hasCommitIntent("please bump
  to 0.8.0")`, `hasCommitIntent("I just committed; now push it")`,
  and `hasCommitIntent("/commit")` all return true. Substrings outside
  the verb stem (`"committee"`, `"pushcart"`, `"thinkpad"`) return
  false. Inflected forms (`-ed`, `-ing`, `-s`) of the listed verbs
  match by the stem regex documented in §2.1. *Test:*
  `tests/features/model_recommender/` —
  `CommitIntentExactWordsMatch`, `CommitIntentInflectedFormsMatch`,
  `CommitIntentSubstringRejected`.
- **INV-2** — when `hasCommitIntent(latestUserText)` returns true,
  `score()` returns `Result{tier=Tier::Haiku, reason="commit_intent",
  currentModel=<from transcript or empty>}` directly, bypassing the
  additive ladder. *Test:* `tests/features/model_recommender/` —
  `CommitIntentBeatsPriorRefactoringWindow`: pre-fix RED state is a
  transcript with 20 assistant turns where each turn contains one
  `Edit` tool_use AND one text block containing a plan keyword
  (e.g. "refactor") → `many_writes(+2) + plan_keyword(+2) = sc=+4 →
  Tier::Opus` against current `modelrecommender.cpp:138`. Plus a
  user-turn text `"commit and push"`. Post-fix → `Tier::Haiku`. The
  RED state proves the override discriminates the heavy-window case
  the user actually hits.
- **INV-2a** — `clampToFloor` interaction is exercised by the GATE,
  not the scorer (the scorer never sees the floor). *Test:*
  `tests/features/model_auto_switch/` — `CommitIntentTargetClampedByGate`:
  `Gate{rec.tier=Haiku, floor=Sonnet}` → `decide()` returns
  `tierArg="sonnet"` (Haiku upclamped to Sonnet). Splits INV-2's
  scorer side from the gate's clamp side, matching the existing
  two-bundle split.
- **INV-3** — `weightForTurnIndex(idx, total)` is monotone
  non-decreasing in `idx`; `weightForTurnIndex(0, n) == 1.0` for all
  `n >= 1`; `weightForTurnIndex(total-1, total) == 3.0` for
  `total >= 2`; `weightForTurnIndex(0, 1) == 1.0`. Undefined for
  `total < 1` (caller short-circuits on empty windows). *Test:*
  `RecencyWeightingMonotone`, `RecencyWeightingEndpoints`,
  `RecencyWeightingSingleton`.
- **INV-4** — recency weighting applies *only* to `fileWriteCount`
  and `avgLen` (count-based); `toolDiversity` and `planKeyword` stay
  unweighted. The weighted-writes threshold is `>= 8` (v1's `>= 4`
  against equal weights summing to 20; v2 weights sum to 40 across a
  full window). *Test:* `WeightedWritesRecentClusterTrips`: 4 writes
  in turn indices {16,17,18,19} (the latest 4 of a 20-turn window) →
  weighted sum = 1+2·16/19 + 1+2·17/19 + 1+2·18/19 + 1+2·19/19 ≈
  2.684 + 2.789 + 2.895 + 3.000 = **11.368 ≥ 8 → many_writes trips**.
  `WeightedWritesOldClusterPasses`: 4 writes in turn indices
  {0,1,2,3} → weighted sum ≈ 1.000 + 1.105 + 1.211 + 1.316 = **4.632
  < 8 → many_writes does NOT trip**. `ToolDiversityUnweighted` and
  `PlanKeywordUnweighted` assert the existing v1 behaviour byte-for-
  byte for those two features (size of `QSet<QString>`, boolean OR
  across walked turns).
- **INV-5** — `latestUserText` is collected in the SAME tail-read +
  reverse walk as the existing assistant-turn extraction; no
  additional `QFile::open` call inside `Result score(...)`. *Test:*
  `tests/features/model_recommender/` — anchored source-grep over
  `modelrecommender.cpp` asserting exactly one `f.open(QIODevice::`
  occurrence between the `Result score(` opener and its matching
  closing brace (sibling functions `thinkingLevelFromLatestUserTurn`
  etc. also open files; the grep scopes to the `score()` body).
- **INV-6** — `ModelAutoSwitch::decide` refuses to act when
  `gate.msSinceLastOverride >= 0 && gate.msSinceLastOverride <
  kOverrideCooldownMs`. The dwell rule
  (`msSinceLastSwitch >= kMinDwellMs`) still applies separately.
  With `msSinceLastOverride == -1` (no override on record), the
  cool-down rule does NOT block. *Test:* `tests/features/model_auto_switch/`
  — `OverrideCoolDownBlocksAct`, `OverrideCoolDownPassesWhenExpired`,
  `OverrideCoolDownIgnoredWhenSentinelMinusOne`.
- **INV-7** — `m_lastOverrideMsByProject` is updated only inside
  `fillPendingLedgerOutcomes` after `computeOutcome` returns with
  `userOverrideWithin5=true && !pending`, AND once at controller
  bootstrap (named site: a new private helper
  `ClaudeStatusBarController::seedOverrideCacheFromLedger(const
  QString &ledgerPath)`, called from `attach()` after the services
  are wired but before the 2 s status timer fires its first tick).
  The 2 s status tick is zero-I/O on the override path. *Test:*
  `tests/features/model_auto_switch_outcome_fillin/` — (a)
  `OverrideCachePopulatedByFillIn`: synthetic ledger with a settled
  `userOverrideWithin5=true` record, invoke
  `fillPendingLedgerOutcomes`, assert `m_lastOverrideMsByProject`
  contains the entry. (b) `OverrideCacheSeededOnConstruction`: same
  fixture ledger, instantiate a fresh controller, call
  `seedOverrideCacheFromLedger(fixturePath)` directly (the helper
  exists precisely so the test injects the path — `defaultLedgerPath()`
  is process-global and unsuitable for tests), assert the cache is
  populated.
- **INV-8** — cool-down scope: an override recorded against
  `project=A` does NOT block an auto-switch decision when the
  focused tab's project is B (the lookup
  `m_lastOverrideMsByProject.value(B, -1)` returns `-1` for the
  unknown key, so `decide()` sees `msSinceLastOverride=-1` and the
  cool-down does not block). *Test:*
  `tests/features/model_auto_switch/` — `OverrideCoolDownIsPerProject`
  (gate behavioural; lives alongside INV-6 where the per-project
  interpretation is a property of how `decide()` reads
  `gate.msSinceLastOverride`, not of how the cache is populated).
- **INV-9** — security: `hasCommitIntent` and `weightForTurnIndex` are
  pure (`const QString &` / primitives in, primitives out), reside in
  `ants_claude_lib` alongside the rest of `ModelRecommender`, and take
  no I/O. The cool-down constants are `constexpr qint64` — no runtime
  mutation surface. Regex match on `latestUserText` is O(N) in
  user-prompt length, bounded by the 512 KB tail-read; no nested
  re-scan. *Test:* `tests/features/model_recommender/` source-grep
  over modelrecommender.{h,cpp}.

## 4. RAM / build cost

- **Per call:** one extra `QString latestUserText` (transient, freed
  on return) — bounded by the existing 512 KB tail-read budget; no
  separate cap needed.
- **Per controller:** one `QHash<QString, qint64> m_lastOverrideMsByProject`
  scalar table — bytes-per-entry on the order of 24–48 bytes (key
  QString + qint64 value + hash overhead); one entry per project the
  user has opted-in for. Effectively single-digit-KB even for a power
  user with 50 projects.
- **Build:** no new TUs. Adds three small pure helpers
  (`hasCommitIntent`, `weightForTurnIndex`, internal latest-user-turn
  collection) + one Gate field + one controller QHash + one
  constructor-time ledger read; ~120 LoC of source + ~250 LoC of
  tests. No new external libraries; `QRegularExpression` is already
  pulled in by ANTS-1888 (`modelrecommender.cpp:8`).

## 5. Out of scope (future)

(These are future re-tune triggers, not open design questions. They
deliberately do NOT use the `OQ-` prefix — that prefix is reserved
for genuine open questions like ANTS-1735 OQ-3.)

- **§5.1 — keyword precision re-tune.** `release` and `merge` excluded
  from v1 (false-positive shape too broad); `bump` included despite
  two false-positive shapes — the literal "bump into the wall"
  (vanishingly rare) and the doc-edit "bump versions in the README"
  (where the edit IS the work, NOT a mechanical follow-through). The
  doc-bump shape is the one most likely to surface a regret. Re-
  evaluate at the next `/bump` release cycle once the ledger has
  ≥ 10 records with `score_reason` containing `commit_intent`; if
  the doc-bump pattern dominates the regrets, narrow `bump` to
  require an adjacent `/git` / `version` token or drop it.
- **§5.2 — cool-down length tune.** 10 min is the default. Note: the
  ledger does NOT distinguish "override of auto-switch" from "user
  re-choosing model deliberately" — both stamp
  `userOverrideWithin5=true`. So a user who flips `/model sonnet` at
  T and `/model haiku` at T+2 min extends their own cool-down to
  T+12. Acceptable (the user is fiddling; leave them alone), but the
  re-tune should not over-fit on this shape. Re-tune at the same
  cycle as §5.1 after the first 5 ledger records with
  `userOverrideWithin5=true`.
- **Read-only intent** (`Read`/`Grep`/`Glob`-only turns → Haiku) —
  same shape as commit-intent but on the assistant-turn tool list,
  not the user text. Deferred to a sibling item if the v2 ledger
  still shows under_route on summarise / find-callers turns after
  several sessions.
- **Build-failure / debug intent** (recent `commandFailed` + user
  retry → Sonnet floor) — needs `CommandFailed` signal correlation,
  which lives outside `ModelRecommender`. Tracked separately if
  needed.
- **Asymmetric dwell** (faster to downgrade than upgrade, or vice
  versa) — orthogonal to this item; revisit only if pingpong is
  observed after v2 ships.
- **Pre-action chip flash on switch** — a small UX improvement,
  not a scorer change; sibling roadmap item.

## 6. Tests

Existing feature-test bundles absorb the new tests (no new bundle
needed):

- `tests/features/model_recommender/` — INV-1, INV-2, INV-3, INV-4,
  INV-5, INV-9 (scorer-side; the natural home, where the existing
  `score()` tests live).
- `tests/features/model_auto_switch/` — INV-6 (pure `decide()`
  cool-down rule).
- `tests/features/model_auto_switch_outcome_fillin/` — INV-7
  (cached-scalar wiring; same fill-in tick the bundle already
  covers) + INV-8 (per-project scope).

Each new test is verified RED against pre-fix source before the green
pass. `CommitIntentBeatsPriorRefactoringWindow` (INV-2) needs a
materialised synthetic transcript with both an assistant-turn body and
a trailing user turn. Reuse the **existing fixture pattern** added by
ANTS-1888 in `tests/features/model_recommender/test_model_recommender.cpp:179+`
(`appendUserTurn(QTemporaryFile &, const QString &text)` writes one
`{type:"user", message:{content:[{type:"text", text:...}]}}` JSONL
line; the array-form of `message.content` matches the existing
scorer fork at `modelrecommender.cpp:206-213`). Extend
`writeSyntheticTranscript` (or add a sibling
`writeSyntheticTranscriptWithUserTail`) to emit N assistant turns
then one user turn in that order, so the scorer's reverse-walk finds
the user line first. The fixture for `CommitIntentBeatsPriorRefactoringWindow`:
20 assistant turns each containing one `tool_use:{name:"Edit"}` AND
one `text` block with a plan-keyword (e.g. `"refactor the cache"`)
to drive sc=+4 → Opus, plus the trailing user turn with text
`"commit and push"`. Pre-fix: returns `Tier::Opus`. Post-fix:
returns `Tier::Haiku`. Hardcode both expectations as literals so the
test doesn't depend on v1 code still existing in the tree.

**Manual recipe.** Preconditions:

- `claude.auto_model_switch = true` (Settings → General).
- A focused tab running Claude Code in a project root the user has
  used before (so the ledger has settled entries — verify with
  `tail -5 ~/.cache/ants-terminal/model-switch-ledger.jsonl`).
- A Claude Code session in that tab that has done ≥ 4 file edits
  in its most recent 20 assistant turns (i.e. you have just
  finished a small refactor).

Recipe:

1. Type "commit and push" and submit. The next 2 s tick (after the
   focused tab returns to `Idle`) should append an auto-switch record
   to the ledger with `to:"haiku"` and `score_reason` containing
   `commit_intent`. Verify with `tail -1 <ledger>`.
2. Now type `/model sonnet` to override. After 30 s the ledger's
   `userOverrideWithin5` flag settles to `true` on that record.
3. Trigger another mechanical turn within 10 minutes: no auto-switch
   record should append (cool-down active). Verify by counting
   ledger lines before/after.
4. Wait > 10 minutes, repeat a mechanical turn: a new auto-switch
   record appends (cool-down expired). ✓

## 7. Cross-doc impact

- **CHANGELOG.md** — feature entry on ship (the user's "Haiku for
  commit/push" intuition validated).
- **CLAUDE.md / docs/subsystems.md** — note `hasCommitIntent` +
  `weightForTurnIndex` alongside the existing `modelrecommender`
  entry.
- **docs/specs/ANTS-1735-autonomous-claude-code-model-switcher.md** — § 5 "Heuristic breadth" line is
  superseded by THIS spec; amend it to point here in the same commit
  that lands the implementation.
- **ROADMAP.md** — the ANTS-1890 bullet's `Lanes:` list drops
  `modelswitchledger` (read-only consumer, not modified) and keeps
  `modelrecommender`, `modelautoswitch`, `claudestatuswidgets`.

**Layman:** When you type "commit and push" after a long coding
session, Ants should pick the cheap-and-fast model (Haiku) for that
quick chore. Today it doesn't notice the last thing you typed; this
change makes it pay attention. It also backs off politely when you've
just told it "no" (`/model X`), so it doesn't immediately re-switch.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-1890-scorer-v2-commit-intent-recency-loop-log.md`](../reviews/ANTS-1890-scorer-v2-commit-intent-recency-loop-log.md).
