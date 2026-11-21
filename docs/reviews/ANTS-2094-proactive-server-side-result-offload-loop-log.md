# ANTS-2094 — Proactive server-side result offload (observation masking) — review loop log

Record for [`../specs/ANTS-2094-proactive-server-side-result-offload.md`](../specs/ANTS-2094-proactive-server-side-result-offload.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-16)** — 2 independent cold reviewers (accuracy lane +
  completeness/testability lane). Verified findings folded:
  - **HIGH** — `read_spill` handler home was wrong: corrected from
    `cmdReadSpill` in `claudeintegration.cpp` to `RemoteControl::cmdReadSpill`
    in `remotecontrol.cpp`, dispatched via `rcDelegate` in mainwindow
    (verified against `cmdReadRegion` at `remotecontrol.cpp:8028` /
    `mainwindow.cpp:4888`). INV-10 updated.
  - **HIGH** — misattributed `allow_outside_project` as a `read_region`
    knob; it is the test-audit verb's arg → `validatePath(allowOutsideRoot)`
    (`pathvalidation.h:50`). § 2.4 reworded.
  - **HIGH** — spill-write failure mode was undefined; added fail-open
    contract (§ 2.2) + INV-11.
  - **HIGH** — `line_count` / line-range re-read is meaningless for the
    single-line compact-JSON bodies these verbs emit; switched `read_spill`
    to byte-offset paging, dropped `line_count` from the envelope. § 2.3,
    § 2.4, INV-6 updated.
  - **MEDIUM** — evicted-handle race unacknowledged: added the
    recoverable-`not_found` note (§ 2.5) + INV-5 clause.
  - **MEDIUM** — concurrency posture unstated: added single-threaded
    dispatch note (§ 2.5, cites ANTS-1360 INV-10).
  - **MEDIUM** — clamp ranges + char-boundary head truncation had no test
    surface; added INV-12.
  - **MEDIUM** — `bytes` field unqualified; defined as raw-body UTF-8 length
    (§ 2.3).
  - **LOW** — cache-dir idiom shown as the bare enum; corrected to the
    `QStandardPaths::writableLocation(...)` call form (§ 2.2).
  - **LOW** — `not_found` vs `not_cached` choice now justified inline
    (§ 2.4).
  - Unverified/dismissed: none — every finding held against current source.
- **Loop 2 (2026-06-16)** — same 2 reviewers, cold (no briefing on loop-1
  fixes). No loop-1 finding resurfaced (the fixes held). New verified
  findings folded:
  - **HIGH** — sub-head body edge: a body in `[threshold, headBytes]` is
    offloadable yet shorter than the head cut; defined `head_truncated:false`
    + `head==body` (INV-2). *(Reversed in Loop 3: the head-size guard makes
    this case unreachable, so `head_truncated` is now always true.)*
  - **HIGH** — `read_spill` edge cases (`offset >= total_bytes`,
    `max_bytes:0`, zero-byte body) were undefined; added to INV-6 + § 2.4.
  - **MEDIUM** — 24 h sweep had no named function/call-site; named
    `mcp::spillSweep()` + its session-start call-site (§ 2.5, INV-7).
  - **MEDIUM** — INV-9 used a non-falsifiable `≪`; tightened to a strict
    `out_bytes < bytes` predicate + anchored the byte-capture/`recordDispatch`
    lines (8884-8886 / 8894).
  - **LOW** — corrected the wrap insertion line ref (8856 is the wrap
    block header; the `wrapMcpData` call is 8872; `appendReadHints` ends
    8854) (§ 2.1).
  - **LOW** — noted the offload eligible-set deliberately differs from
    `isFieldProjectionTool` (§ 2.1).
  - **LOW** — §6 now maps which tests assert which INVs.
  - **LOW** — dropped the `ANTS-MMMM` placeholder for a sign-off-gated
    follow-up phrasing (§ 5).
  - Dismissed (unverified): the "ANTS-2085 attribution" flag on
    `offloadRequested` — the per-call-wins-else-`terseDefault()` resolution
    *is* ANTS-2085 (comment at `claudeintegration.cpp:8830`), so the cite
    is correct; no change.
- **Loop 3 (2026-06-16)** — same 2 reviewers, cold. No loop-1/2 finding
  resurfaced. All new findings MEDIUM/LOW (no CRITICAL/HIGH). They
  converged on one root cause — *offloading a body that fits inside the
  returned head saves nothing* (the envelope ends up ≥ the original).
  Folded:
  - **MEDIUM** — added the head-size guard `bodyBytes > offloadHeadBytes()`
    to the offload trigger (§ 2.1); makes offload a guaranteed net saving,
    keeps INV-9's strict `out_bytes < bytes` always true, and removes the
    degenerate sub-head / zero-byte cases. INV-1/INV-2 updated; the
    loop-2 `head_truncated:false` case is now unreachable (offloaded ⟹
    head_truncated always true) and was simplified out.
  - **MEDIUM** — `read_spill` paging contract pinned: advance by the
    **returned** `offset + bytes`, not the requested `offset + max_bytes`
    (char-boundary cut can shorten a slice) (§ 2.4, INV-6).
  - **MEDIUM** — reworded the "idempotent regardless" overclaim (§ 2.5):
    write-race safety comes from single-threaded dispatch, not
    content-addressing.
  - **MEDIUM** — pinned the `bytes == threshold` offload-fire boundary
    (`>=`, not `>`) as an explicit test (INV-1).
  - **MEDIUM** — INV-6 zero-byte clause was unreachable (a zero-byte body
    is sub-head, never spilled); replaced with the head-guard rationale.
  - **LOW** — eviction never drops the just-written handle (sub-second
    mtime-tie carve-out, § 2.5).
  - **LOW** — INV-11 now asserts `QSaveFile` leaves no orphan temp on a
    failed write. (The runtime no-stray-file assertion actually landed later,
    in ANTS-3554's `Inv11FailOpenFaultInjection`; the pre-3554
    `Inv11FailOpenWiring` was source-scrape only.)
  - **LOW** — INV-9 test scrapes by symbol (`mcp::offloadBody` precedes
    `recordDispatch(`), not by drift-prone line numbers.
  - **LOW** — INV-6 asserts `bytes == len(content)`.
  - Dismissed: `mainwindow.cpp:4888` "mid-statement" cite (harmless, it's
    the `rcDelegate` line); the missing `mcp-spill` mcp-caches row (§ 7
    already tracks it as a same-commit ship task, not a spec defect).
- **Loop 4 (2026-06-16)** — same 2 reviewers, cold. No loop-1/2/3 finding
  resurfaced; all new findings MEDIUM/LOW (no CRITICAL/HIGH). Folded:
  - **MEDIUM** — `read_spill` `caller_cwd` corrected from Required to
    **Optional**: the spill store is global/content-addressed (`~/.cache`,
    not project-scoped), so it routes by `handle`, not project root
    (§ 2.2, § 2.4, INV-10). Added the cross-project-leak rationale (handle
    = `sha256(content)`, so naming a handle implies having had the
    content).
  - **MEDIUM** — INV-9 conflated the response-body `rawBytes` local with
    the recorded raw-bytes arg, which is actually `buf.size()` (the inbound
    *request*, `:8895`). Re-pinned the wire saving to `out_bytes` (`:8884`,
    post-offload) and noted raw-bytes is offload-independent.
  - **MEDIUM** — `read_spill` negative `offset`/`max_bytes` now refuses
    `bad_args` (§ 2.4, INV-6).
  - **MEDIUM** — added an INV-2 clause asserting the envelope's `hint`
    field names `read_spill` (the recovery contract was unverified).
  - **LOW** — `setOwnerOnlyPerms(const QString&)` is applied to the final
    path *after* `commit()` (QSaveFile temp-file perms don't survive the
    rename) (§ 2.2).
  - **LOW** — INV-5 test now names the uppercase-hex / 63-65-char handle
    cases (locks the lowercase-only, exact-64 regex intent). (These were
    never added as behavioural tests — they are unreachable at the free
    `readSpill`/`readSpillRows` layer, which returns `not_found`, not
    `bad_args`; the regex intent is covered only by the `Inv10ReadSpillWiring`
    source-scrape. Corrected in the ANTS-3554 pass.)
  - **LOW** — added a worked `head` fragment to § 2.3 (concrete example).
  - **LOW** — cross-linked the Loop-2 `head_truncated:false` log entry to
    its Loop-3 reversal.
  - Dismissed: none.
- **Loop 5 (2026-06-16)** — same 2 reviewers, cold. Both affirmed the
  spec is implementable and self-consistent, **no CRITICAL/HIGH**, ready
  for sign-off pending the OQ-1 default decision. No prior-loop finding
  resurfaced. Minor polish folded:
  - **MEDIUM** — reworded §2.1's "guard is normally redundant" so it no
    longer undercuts the head guard's load-bearing role (it is mandatory
    because the clamp ranges overlap; INV-2/INV-9 depend on it).
  - **MEDIUM** — INV-9 now states all spec line numbers are illustrative
    as-of-date and the test asserts ordering by *symbol*, resolving the
    "scrape-by-symbol vs hard-coded literals" tension.
  - **MEDIUM** (implementer subtlety) — eviction carve-out keys on
    **handle identity**, not mtime; an idempotent re-spill (INV-8) keeps
    the old mtime, so a re-spill touches the mtime and the carve-out names
    the current handle (§ 2.5, INV-7).
  - **LOW** — INV-6 pins the `max_bytes` clamp vs offset-past-end ordering.
    (The wording it added — `max_bytes:0` "returns 1 byte in range" — never
    matched `readSpill`; corrected in the ANTS-3540 amendment: `max_bytes:0`
    falls back to the 512 KiB default page.)
  - **LOW** — tightened the `mainwindow.cpp` cite to `4886-4888`
    (registration through dispatch).
  - Dismissed (non-defects): §7-vs-§2.5 category wording (§7 already
    carries the full "Cold/Orphan, never Shadow; GC-sweep candidate"
    descriptor); INV-6/§2.4 `truncated` stated in both (INV-6's `iff` is
    the single normative form — acceptable surface/invariant redundancy).

### ANTS-3538 amendment loops (2026-07-16)

Scope: only the § 2.3.1 structured preview + INV-13 + the touched
§ 2.3/§ 4/§ 6/§ 7/Status lines (loops 1–5 above are the accepted base). Two
independent cold reviewers per loop (accuracy/invariant-preservation lane +
completeness/edge-case/testability lane); loops run cold (no briefing on
prior-loop fixes). Converged loop 6 to polish; INV-2 / INV-9 / INV-12
preservation affirmed **sound** by both reviewers.

- **Loop 1** — no CRITICAL/HIGH. **MEDIUM** ×2: the tie-break "first-seen
  member order" is unimplementable under the prescribed `QJsonObject` parse
  (Qt sorts keys) → reworded to lexicographic/`QJsonObject` order; the
  tabular (ANTS-2090) interaction was unaddressed → detection pinned
  root-only/no-recursion + tabular consequence documented. **LOW** ×5:
  restated "non-empty" in the dominant-array def; added a
  `kStructuredParseMaxBytes` (1 MiB) parse cap for RAM; corrected the
  `kStructuredOverhead` rationale; bumped it 512→640 B; added the
  raw-vs-escaped `head` caveat.
- **Loop 2** — **MEDIUM** ×1: the budget-omit clause left the *other three*
  structured fields undefined and §2.3.1↔INV-13 worded it inconsistently →
  pinned all-four-or-none omission. **LOW**: "verbatim" contradicted the
  deep-equals contract; double-counted array punctuation; corrected the
  `hint` byte figure; pinned the `kHeadRowsMax` boundary; §2.3 size bound
  precision.
- **Loop 3** — **MEDIUM** ×2: the constant-slack INV-9 proof was defeatable
  by an escape-heavy `head` at a pathological config → **replaced with an
  empirical measure-the-envelope budget** (drop `kStructuredOverhead`),
  making INV-9 correct-by-construction; the omission edges had zero test
  coverage → added them to INV-13.
- **Loop 4** — **HIGH** (scoped): the "INV-9 holds regardless of config
  extremes" claim over-reached — the omission fallback reduces to the
  pre-3538 envelope, which has a pre-existing head-guard-vs-overhead hole →
  scoped the claim to the structured path and **filed the pre-3538 hole as
  ANTS-3540** (out of amendment scope to fix). **MEDIUM** ×3: the
  `head_rows_truncated` true→false width flip could eat the `−1` margin →
  measure `S0` at the wider `false`; the greedy `bytes−S0−1` budget defeated
  the feature's purpose (≈`bytes−1` preview) → **reintroduced the
  `offloadHeadBytes()` preview cap**; INV-13 restated `bytes−S0` vs
  §2.3.1's `bytes−S0−1` → single source of truth.
- **Loop 5** — no CRITICAL/HIGH; INV-2/9/12 affirmed sound. **MEDIUM** ×2:
  the "tabular ⇒ byte-head fallback" claim was over-broad (a tabular
  `find_sources` still exposes the unpacked `unmatched_terms` string array,
  legitimately previewed) → narrowed the claim + fixed the test; two
  defined omission edges (over-parse-cap, unparseable body) lacked
  assertions → added. **LOW**: budget-driven truncation under-asserted.
- **Loop 6** — no CRITICAL/HIGH/MEDIUM from the accuracy lane (all four
  dimensions affirmed sound, arithmetic verified); completeness lane
  surfaced **polish only**: §6↔INV-13 test-list drift (sibling-preview
  case), missing tie-break test, and the "same body's non-array fallback"
  phrasing named a non-producible target. Folded all; **polish
  convergence** — stopped.

### ANTS-3540 fix (2026-07-16)

Closed the pre-3538 base-envelope INV-9 hole filed in Loop 4 above.
`mcp::offloadBody` now measures the finished envelope and fails open
(returns the body unchanged, per INV-11) whenever the envelope would not be
strictly smaller than the body — so the head-guard-vs-fixed-overhead
head≈threshold case can no longer offload to a net loss. Contract unchanged
(INV-9 was always the guarantee; the code now honours it at every config);
this amendment only refreshed the § 2.3.1 exposure note, § 2.1, and INV-9's
parenthetical to match. Covered by the reproduce-first INV-9 conformance
case `Inv9BaseEnvelopeNeverExceedsBody` (the fail-open is a facet of INV-9,
not a new invariant): a scalar body just over a 16384-byte head was RED
(16720 B envelope for a 16411 B body) before the fix, GREEN after.

### ANTS-3545 amendment loops (2026-07-16)

Scope: only the new § 2.4.1 (`read_spill` row-paging) + INV-14 + the shared
`dominantArrayKey` note in § 2.3.1 + the touched § 4/§ 5/§ 6/§ 7 lines
(loops 1–6 above + the ANTS-3538/3540 amendments are the accepted base). Two
independent cold reviewers per loop (accuracy/invariant-preservation lane +
completeness/edge-case/testability lane); loops run cold (no briefing on
prior-loop fixes).

- **Loop 1** — both lanes independently flagged the two load-bearing gaps.
  **HIGH** ×2: the `read_spill` `inputSchema` is `additionalProperties:false`
  and never had `row_offset`/`row_count` added → the mode was unreachable
  (feature ships dead) — added the § 2.4.1 Routing paragraph + a § 7
  `claudeintegration.cpp` schema line + an `Inv14RowModeWiring` scrape; and
  the `row_count` arg contract self-contradicted (`<=0 ⇒ default` vs
  `negative ⇒ bad_args`) → pinned "`0`/omitted ⇒ default, negative ⇒
  `bad_args`" per the byte-mode precedent. **MEDIUM** ×5: the 4 MiB output
  ceiling + single-oversized-row forward-progress was **dead** (1 MiB parse
  cap < 4 MiB ceiling, so it can never bind) → removed it, stating the parse
  cap is the sole bound; the row-mode refusal list dropped the inherited INV-5
  `not_found`/handle-`bad_args` → added an "Inherits INV-5" clause; the new
  `too_large`/`not_array` codes were not scheduled for `mcp-error-codes.md` →
  added the § 7 taxonomy line (category 2); the `cmdReadSpill` mode-router was
  unspecified/untested → specified routing + `Inv14RowModeWiring`; INV-14's
  continuity test never exercised a short/partial final page → added an
  N-not-a-multiple-of-K final-partial-page assertion. **LOW** ×3:
  `kSpillRowsDefault` value unstated → pinned = 100; INV-14 must-fail-first
  undefined for a not-yet-existing function → noted the link-time-absence RED
  (as INV-5/6/13); envelope `key` vs preview `head_rows_key` naming → clarified
  same-value/shorter-name. **INFO** ×1: § 4 transient peak ~6→~7 MiB (the
  extra re-serialize buffer). All verified against `mcpspill.{h,cpp}` /
  `remotecontrol.cpp` / `claudeintegration.cpp`; none dismissed.
- **Loop 2** — no CRITICAL/HIGH; no Loop-1 finding resurfaced (the schema +
  `row_count` fixes held). **MEDIUM** ×3: the slice formula `min(row_offset +
  effective_count, total_rows)` was integer-overflow-prone (an addition of two
  caller-controlled values) → rewritten as `effective_count = min(row_count,
  total_rows − row_offset)` (subtraction, the byte-mode `total − offset`
  shape) with a past-end guard first; INV-14 listed the negative-arg `bad_args`
  *after* `not_array` while § 2.4.1 makes it a `cmdReadSpill` pre-gate →
  re-ordered the INV-14 refusal list to `cmdReadSpill`-then-`readSpillRows`
  (handle-regex + negative-arg `bad_args`, then `not_found`, `too_large`,
  `not_array`); the negative-arg refusal + row-vs-byte precedence had no test →
  moved them into the `Inv14RowModeWiring` handler case. **LOW** ×4: the
  handle-regex `bad_args` was mis-attributed to `readSpillRows` (it is a
  `cmdReadSpill` gate; `readSpillRows` returns `not_found` for a bad handle) →
  corrected; the mode-trigger predicate was unpinned → pinned to `isDouble()`
  (a present-but-non-numeric row arg does not trigger row mode); the
  `readSpillRows` result struct was unspecified → enumerated `SpillRows` in
  `mcpspill.h` (parallel to `SpillSlice`); the row-mode tabular interaction was
  inferable only → stated explicitly (fully-tabular → `not_array`, sibling
  scalar array paged) + a root-array `not_array` note. **INFO** ×2: § 4 peak
  ~7→~8 MiB (raw read buffer counted); § 7 `tool_info`/`description` must tell
  the agent row mode returns a different envelope shape. **Dismissed
  (unverified):** "`readSpillRows` mislocated to `remotecontrol.cpp`" — the
  cited line 394 already reads `src/mcpspill.cpp` (reviewer misread); no edit.
- **Loop 3** — no CRITICAL/HIGH; the Loop-2 overflow/ordering/predicate fixes
  held. **MEDIUM** ×3: `readSpill` does an unconditional `readAll()`, so a
  `readSpillRows` that mirrored it would load a multi-MB body into RAM only to
  refuse `too_large` → pinned a **stat-before-read** (`QFileInfo::size()`)
  gate + corrected § 4's inaccurate "byte-mode carries the same read buffer"
  claim; `Inv14RowModeWiring` cannot *behaviorally* test the handler (the
  bundle links `ants_core_lib`, not `RemoteControl`) → **moved the negative-arg
  `bad_args` gate into `readSpillRows`** (directly unit-testable) and honestly
  downgraded the routing/precedence coverage to source-scrape-only; the
  per-response offload `hint` (`mcpspill.cpp:141`) wasn't scheduled to mention
  row paging → added a § 7 line (discoverability). **LOW** ×3: the existing
  unconditional byte-negative `bad_args` pre-gate had to move **into** the byte
  branch (else a mixed request with a stray negative byte arg refuses instead
  of running row mode); § 7 wrongly listed `payload_too_large` as tabulated
  (only `result_too_large` is) → corrected; the schema `minimum:0` makes the
  negative gate direct-call-only → resolved by the `readSpillRows`-owned gate
  (the bundle can drive it). **INFO** ×2: pinned the `0`/omitted → default
  application order (negative-check before default, in `readSpillRows`) and the
  `readSpillRows` signature + `cmdReadSpill`-owned `error`/`hint` strings. All
  verified against `mcpspill.{h,cpp}` / `remotecontrol.cpp` /
  `claudeintegration.cpp` / `mcp-error-codes.md`; none dismissed.
- **Loop 4 — converged (polish).** No CRITICAL/HIGH; both lanes affirmed the
  core design (shared `dominantArrayKey`, half-open overflow-safe arithmetic,
  refusal ordering + gate locations, the stat-before-read RAM bound, and the
  honest behaviorally-testable-vs-source-scrape split) **sound**. The residue
  was refinement, all folded: **MEDIUM** ×1 — the row-paging `hint` was gated
  on `head_rows_key` presence, but § 2.3.1 omits that key for a large-first-row
  body that is still row-pageable (and where byte paging lands mid-row) →
  re-gated the hint on `domCount > 0` (computed before the head_rows budget
  check; stays byte-only for `> 1 MiB` and non-array bodies). **LOW** ×2:
  pinned the refusal-envelope shape `{ok:false, code, error, hint}` (the
  row-mode `hint` on `too_large`/`not_array` is new vs byte mode) + an INV-14
  assertion for it (the `hint` is built in `cmdReadSpill`, not on the
  `SpillRows` struct `readSpillRows` returns, so the assertion is a
  source-scrape in `Inv14RowModeWiring`, not a direct-call check — corrected
  in the ANTS-3554 pass); noted the `kSpillRowsDefault = 100` default page is
  row-count-bounded and can exceed byte mode's 512 KiB default for large-row
  bodies (intentional, still parse-cap-bounded). **INFO** ×3: added the § 5
  row-mode out-of-scope bullet (field projection / dedup / hard row cap — the
  scope line claimed § 5 was touched); clarified INV-14's must-fail-first is a
  **link** RED (symbol absent) vs INV-5/6/13's *assertion* RED; the `too_large`
  → byte-paging redirect still `readAll()`s the body but avoids the parse (the
  dominant RAM cost — pre-existing base behaviour, § 4 acknowledges it, out of
  scope). **Converged** — no substantive (structural/mechanical/architectural)
  finding remained; the pass surfaced only these refinements, all fixed.

### ANTS-3552 amendment loops (INV-1/INV-6 test-notes + `shouldOffload`)

- **Loop 1 — amendment clean; pre-existing drift surfaced.** Scope: the
  ANTS-3552 edits — INV-1's test-note rewrite (the `>=`threshold / `>`head
  boundary is now the extracted `mcp::shouldOffload(bodyBytes)` predicate, gated
  at the dispatch; behaviourally tested by `Inv1ShouldOffloadBoundary` /
  `Inv1OffloadBodyHasNoInternalThresholdGuard` / `Inv1DispatchUsesShouldOffload`)
  and INV-6's test-note update (`max_bytes<=0` → 512 KiB default page +
  `cmdReadSpill` negative-arg `bad_args` gate, `Inv6ReadSpillMaxBytesDefaultsToPage`
  / `Inv6ReadSpillNegativeArgGateAtDispatch`). Two independent cold lanes
  (accuracy / testability) **both affirmed the amended INV-1/INV-6 sections
  accurate — 0 findings on the changed text**, verified against `mcpspill.{h,cpp}`,
  `claudeintegration.cpp`, `remotecontrol.cpp`, and the tests. The run
  additionally surfaced **pre-existing** test-coverage over-claims elsewhere
  (INV-5 handle-format edges, INV-7 re-spill-under-eviction, INV-11 fault-injection
  vs source-scrape, INV-14 `too_large` / `not_array` / `hint`, § 6 marginal-body
  cross-ref, and the ANTS-3540 net-saving fail-open's undisclosed disk-write +
  eviction side-effect) plus two stale cold-eyes-log claims (Loop-3 INV-11
  runtime assertion, Loop-4 INV-14 `hint` assertion) — **none introduced by this
  amendment**. Not fixed here (out of ANTS-3552 scope; each needs the missing
  test added alongside the note correction to avoid churn); tracked as **ANTS-3554**
  for a dedicated doc+test pass with its own cold-eyes. `CRITICAL 0 · HIGH 3 ·
  MEDIUM 4 · LOW 1 · INFO 1` — all pre-existing, 0 on the amended sections.

### ANTS-3554 amendment loops (test-note corrections + backfilled coverage)

Scope: the ANTS-3552 cold-eyes had surfaced pre-existing test-note over-claims
across INV-5/7/9/11/14 + § 6 and two stale cold-eyes-log claims. ANTS-3554
corrects each note **and** adds the coverage it claimed, in one pass (avoiding
the note-vs-test churn a docs-only fix would leave). Behaviour was already
correct + shipped (ANTS-3540/3545) — this is coverage backfill, not a bug fix.
Two independent cold reviewers (accuracy / testability lanes), shared-read-cache,
briefed cold (no fix list).

- **Added tests** (`test_mcp_result_offload.cpp`): `Inv11FailOpenFaultInjection`
  (runtime fail-open — spill dir under a regular-file parent so `mkpath` fails
  regardless of uid → `offloadBody` returns the body verbatim, no stray file);
  `Inv14RowModeRefusalEdges` (the `readSpillRows` stat-before-read `too_large`
  gate + the empty-array-member and bare-root-array `not_array` shapes); and, in
  `Inv14RowModeWiring`, source-scrapes of both `cmdReadSpill` redirect-`hint`
  literals. **Corrected notes**: INV-5 (the `^[0-9a-f]{64}$` `bad_args` gate is a
  `cmdReadSpill` scrape via `Inv10ReadSpillWiring`; the free functions return
  `not_found`), INV-7 (dropped the un-exercisable re-spill-under-eviction clause
  — an idempotent re-spill refreshes mtime, so the carve-out can't conflict),
  INV-9 (disclosed the ANTS-3540 fail-open's write-then-evict disk side-effect),
  INV-11 (names the fault-injection test; § 6 marginal-body omission-edge dropped),
  INV-14 (`hint` is a `cmdReadSpill` source-scrape, not a `SpillRows` direct-call
  assert). **Corrected log claims**: Loop-3 INV-11 and ANTS-3545 Loop-4 INV-14-hint
  annotated in place.
- **Loop 1 — converged (polish).** No CRITICAL/HIGH/MEDIUM. Both lanes verified
  the corrected INV-5/7/9/14 notes, § 6, the added tests, and the two annotated
  log claims accurate against the tests + `mcpspill.cpp` / `remotecontrol.cpp`.
  **LOW** ×2, both fixed: (1) the just-corrected INV-11 note still over-claimed —
  it said `Inv11FailOpenWiring` scrapes "three" fail-open branches but the test
  scraped two, and the write path actually has **four** `return body;` exits →
  added the `open` + short-write scrapes and reworded the note to "all four
  (`ensurePrivateDir` / open / short-write / `commit`)"; (2) a **third** stale
  cold-eyes-log claim missed by the ANTS-3552 sweep — the original Loop-4 "INV-5
  test now names the uppercase-hex / 63-65-char cases" (no such behavioural test;
  unreachable at the free-function layer) → annotated in place like its two
  siblings. **INFO** ×1 (not fixed): the fault-injection fires in the
  `ensurePrivateDir` branch, so the `QSaveFile`-commit-orphan path is only
  vacuously exercised — acknowledged, no test induces a commit failure directly.
  `CRITICAL 0 · HIGH 0 · MEDIUM 0 · LOW 2 · INFO 1`. Both LOWs are doc-accuracy
  polish, both fixed → **converged** (no substantive finding); full suite green
  at 2710.

### ANTS-4692 amendment loop (2026-08-26)

**Loop 1 — 3 cold lanes, genre pinned `spec`, one shared byte-stable packet
(the `offloadBody`, `isProtectedCompactKey`, `mcpprojection.h` and
`check_sync` windows). Q1 1 · Q2 3 · Q3 5 — 9 verified, 9 fixed, 0
dismissed.** Run capped at 1 loop by argument, not by convergence; the
remaining tail is empty because every verified finding was fixed, and the
spec's own cap is 2.

**All three lanes independently found the same two**, which is the strongest
signal in the run. `kPreservedFieldMaxBytes` was named three times and given
neither a value nor a home file, while every sibling constant in this spec is
pinned both ways — so the wire contract other verbs' callers branch on
differed per builder, and INV-15's own test clause could not be written. And
`preserved_omitted` was specified as naming "every member not copied", which
by the Skipped bullet directly above it includes the dominant array and every
offload-owned key: one builder emits `preserved_omitted:["bullets"]` on every
array body, another only on real losses, and both satisfy the sentence.

**The Q1 was pre-existing and is ANTS-4704's subject, fixed here rather than
left standing in a document being amended.** § 2.3.1 and INV-13 both said a
single-oversized-row body emits **no** structured preview and is
"byte-identical to the non-array fallback". ANTS-4397 made that case emit a
shape summary, and ANTS-4474 and ANTS-4519 extended it; none of the three was
ever recorded here. An implementer would have written the required omission
test, watched it fail, and either deleted the shape preview or weakened the
test. The family is now documented in § 2.3.1 and the false claim is deleted
from both places it stood.

**The sharpest Q2 was against the new text and only one lane saw it**:
§ 2.3.2 claims INV-9 holds "by construction" because preservation is measured
before the row-budget probe, while § 2.3.1's explicit `S0` recipe enumerates
the fields that probe is built from and does not include the preserved
members. A builder following the recipe overshoots `headRowsBudget`, the
finished envelope reaches `bytes`, and the ANTS-3540 fail-open silently
disables offload for exactly those bodies. § 2.3.1's recipe now names them.

**Two more Q2s were enumerations that disagree**: § 6 still described the
bundle as one case per invariant "INV-1..INV-14" after § 3 gained INV-15, so a
bundle built from § 6 ships no preservation test and INV-15 gets no RED step;
and § 7's cross-doc impact had been extended for ANTS-3538 and ANTS-3545 but
not for the preserved fields, so the behavioural notes other sessions read
would not say that an absent field now means "dropped, and listed".

**One Q3 was a real ordering gap the doc created for itself**: the budget is a
*running* total, which makes the surviving set order-dependent, and no
iteration order was stated — while INV-15's test spoke of a budget "already
spent by earlier members". Protected-first, then `QJsonObject` key order, is
now pinned.

**Open questions, all resolved clean and none a finding.** Two lanes could not
confirm from the packet that `mcpspill.cpp` can see a symbol published on
`mcpprojection.h`; `CMakeLists.txt` lists both sources in `ants_core_lib`, so
the instruction is buildable. One asked whether the shape preview is specified
in some other document — it is not, which is the Q1 above. One asked what
`source`'s values mean; a packet gap, not a defect.

**Filed, not fixed:** a lane's finding that the dominant array is skipped from
preservation while § 2.3.1 may emit no `row_count` for it is real, and the
residual is a code defect in the ANTS-4397 arm rather than a contract
question — ANTS-4705.

### ANTS-5364 implementation row (2026-10-03) — no reviewer dispatched

Origin: implementation, not a review loop. A reporter's
`roadmap_query` call with `max_body_bytes:20000` spilled a
19,617-byte reply. INV-1 now gates on the call's own threshold,
`mcp::offloadThresholdFor`: an explicit `max_body_bytes` on
`roadmap_query` raises it to that value plus 4 KiB, at most 64 KiB, and
never lowers it (user decision 2026-10-03). § 2.1's pseudocode and INV-1
amended to match; `Inv1ExplicitBodyCapRaisesThreshold` added. Records
code built and tested in the same change, so no gate (CLAUDE.md rule 14,
the code-already-exists exception).

### ANTS-4954 implementation row (2026-11-03) — no reviewer dispatched

Origin: implementation, not a review loop. A 19,432-byte read_region
reply spilled under the 16 KiB default, far below that verb's own
max_bytes. INV-1 now gives read_region the 64 KiB ceiling ANTS-5291
gave read_regions; INV-2 and § 2.3 add `threshold_bytes` to the
envelope, and § 2.3.2 lists it as offload-owned.
`Inv1ReadRegionsUsesTheCeiling` extended and
`Inv2EnvelopeNamesItsThreshold` added. An amendment, so no gate
(CLAUDE.md rule 14).
