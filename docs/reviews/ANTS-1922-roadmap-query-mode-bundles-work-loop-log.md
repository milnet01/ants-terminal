# ANTS-1922 — `roadmap_query mode:"bundles"` (work-bundle discovery) — review loop log

Record for [`../specs/ANTS-1922-roadmap-query-mode-bundles-work.md`](../specs/ANTS-1922-roadmap-query-mode-bundles-work.md) § Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-06-20) — 2 independent cold reviewers

Verified findings folded (all against current `src/remotecontrol.cpp`):

- **C1 (CRITICAL, both reviewers)** — `rcComputePossibleDuplicates` takes `QVector<RoadmapDialog::BulletRecord>` (verified `remotecontrol.cpp:970-1024`), not the cached `QJsonArray`; the spec's "direct reuse, no new similarity code" feeding it a `QJsonArray` subset would force a parse and break the no-parse contract. **Fix:** §2.4 refinement 1 now reuses the extracted `rcHeadlineJaccard` on cached token sets (≥0.60 bar), explicitly *not* `rcComputePossibleDuplicates`. Reconciled with INV-8.
- **H1 (transitivity)** — INV-2's negative clause wasn't guaranteed by connected components. **Fix:** INV-2 reworded to "no direct edge nor transitive path"; transitive closure stated; test now asserts an A–B–C chain.
- **H2 (`blocks ` false-positive)** — bare verb `blocks ` would flag actionable items as blocked. **Fix:** `blocks ` dropped from the marker set (markers are now directional only); INV-5 asserts "blocks the cursor" does *not* set `blocked`.
- **H3 (no-op ≥60 filter)** — the old "≥60" filter was redundant against `rcComputePossibleDuplicates`'s built-in 0.60 gate. **Fix:** dissolved by the C1 rework — the 0.60 bar is now applied explicitly by the spec on the Jaccard ratio.
- **M1 (phantom stop-word filter)** — `rcNormaliseHeadline` does no stop-word stripping (verified `remotecontrol.cpp:932-958`). **Fix:** §2.3 states plainly that the ≥2-token floor is the entire defence; `bundle_label` defines its own small stop-word set.
- **M2 (`items[]` order)** — union-find walk order unspecified → flaky envelope. **Fix:** items sorted by id asc (§2.4); INV-11 added (byte-stable envelope).
- **M3 (empty active set)** — unaddressed. **Fix:** INV-10 added.
- **M4 (truncation vs INV-3)** — `truncated`/`next_offset` uncovered + conflicted with `sum==active_total`. **Fix:** §2.5 reconciles (truncation drops whole bundles; INV-3 scoped to `truncated:false`); INV-12 added.
- **LOW** — allow-list cite tightened to `2791-2793`; dup-detector cite to `970-1010`; redundant "1-item bundles sort last" clause removed; cold-eyes loop log added (this section).

Unverified / dismissed: M3-A (`headline_oneline` always present — confirmed always set on cache population, no caveat needed). INFO: `Layman:` line omitted (internal MCP verb, no user-facing UI — standard §5.2 N/A).

### Loop 2 (2026-06-20) — 2 independent cold reviewers, briefed identically to loop 1

No CRITICAL; **no loop-1 finding resurfaced** (the loop-1 fixes held). New findings folded:

- **H1 (truncation mechanism)** — the spec claimed parity with `section_index`'s truncation, but that path uses `PaginationEngine::pageBullets`/`measureCutPoint` (flat-array `offset`/`limit`/`next_offset`, `kSoftCapBytes=20480`), which does not map onto variable-size bundles. **Fix:** §2.5 now describes a bespoke whole-bundle measure loop, states bundles mode does *not* reuse the flat-array pager, and documents the v1 no-`next_offset` limitation (dropped bundles are the smallest/lowest-priority; pagination deferred).
- **H2 / M3 (score rounding)** — `round(jaccard*100)` was under-pinned; the existing dup-detector uses `static_cast<int>(jac*100+0.5)` (`remotecontrol.cpp:1002`). **Fix:** §2.4 + INV-4 pin the canonical rounding so the score is byte-stable (INV-11); INV-4 test gains a sub-0.60 boundary case.
- **M2 (label sort key)** — the >3-qualifying-tokens selection was ambiguous. **Fix:** §2.4 + INV-9 pin "(frequency desc, token asc), take first 3."
- **M1 (label empty-lanes fallback)** — INV-9's "always non-empty" was untested at its weakest rung. **Fix:** INV-9 test now exercises the all-empty-lanes/no-majority bundle that forces the lowest-id fallback.
- **M2-A / M4-A (gate-note line def + stop-word stance)** — **Fix:** §2.4 refinement 2 defines "line" (`\n`-split), the case-folded-substring match predicate (+ the `until`+`lands`/`ships` rule), and the hard-char-cut boundary caveat; the label stop-word set is explicitly flagged label-only and distinct from the absent tokenisation list.
- **LOW** — dup-detector cite widened `970-1010` → `970-1024` (true function extent).

Dismissed: L2 (mid-line-split marker at the 2000-char boundary — now covered by the §2.4 hard-char-cut caveat). Code-side open questions surfaced (not doc fixes): whether `measureCutPoint` could take pre-grouped units (resolved by going bespoke); eager-vs-lazy token-set build (eager is required since each shipped set is touched once — budgeted in §4); Rule-of-Three check for an existing body-scanner before adding `rcExtractGateNote` (to confirm at implementation).

### Loop 3 (2026-06-20) — 2 independent cold reviewers, briefed identically

No CRITICAL, no HIGH; **no loop-2 finding resurfaced**. Remaining MEDIUM/LOW findings folded:

- **M1-A (INV-12 untestable)** — a source-grep on a variable name doesn't lock the whole-bundle-boundary behaviour. **Fix:** added a test-only `setBundleSoftCapOverride(int)` seam (mirroring ANTS-2154's `setSpillDirOverride`); INV-12 is now a real behavioural test (force a low cap, assert `truncated:true` + intact `items[]` + `sum < active_total`).
- **M1-B (INV-7 conflation)** — INV-7 asserted two claims (status-ignored + case-insensitive lane), tested only one. **Fix:** INV-7 narrowed to status-ignored; new **INV-13** locks the case-insensitive lane-tally with a mixed-case-lane fixture.
- **M2-B (lanes union case-folding)** — emitted `lanes[]` union case-handling was unstated. **Fix:** §2.4 states the union is case-sensitive verbatim; only the label fallback tally folds case.
- **L2-A (lowest-id ambiguity)** — "lowest member id" was ambiguous between numeric and string order (`ANTS-999` vs `ANTS-1000`). **Fix:** §2.5 defines one id-ordering rule (integer-suffix ascending, lexicographic tie-break) used by `items[]` sort, the bundle tie-break, and the label fallback — sealing INV-11.
- **L1 (truncated in INV-1)** — **Fix:** `truncated` added to INV-1's always-present key set.
- **L3-B / L4-B** — **Fix:** the `until`+`lands`/`ships` rule promoted to an INV-5 test case; gate-note pinned to trim-then-cap order.
- **§4** — noted token sets are built per-call (not memoised); follow-on to cache them only if a huge roadmap exceeds the TTL.

Dismissed: L3-A (`Kind: enhancement` confirmed a valid roadmap Kind; trailing-period parse matches the standard's own skeleton). Code-side open question (not a doc fix): confirm at implementation that eager all-bullet tokenisation stays under the 100 ms TTL on the largest real roadmap.

### Loop 4 (2026-06-24) — 2 independent cold reviewers, briefed identically

No CRITICAL, no HIGH; **no loop-1/2/3 finding resurfaced** (every prior fix held). One reviewer raised two HIGH line-citation findings; **both were dismissed on verification** — the spec's cites (mode allow-list `2791-2793`, `bad_mode` refusal-assignment block `2799-2801` with the `return` at `2802`, combo-guard chain `2806-2867`, `section_index` branch `3048`, `rcComputePossibleDuplicates` extent `970-1024`) were all confirmed accurate against current `src/remotecontrol.cpp`; the reviewer's own line-reading had drifted 1–2 lines. Three MEDIUM/LOW clarity findings verified and folded:

- **M1 (kSoftCapBytes locus)** — `kSoftCapBytes = 20480` lives in `namespace PaginationEngine` (`src/paginationengine.h:17`), not `remotecontrol.cpp`, and bundles mode does *not* use the `PaginationEngine` pager — so an implementer could hardcode `20480`. **Fix:** §2.5 now qualifies it `PaginationEngine::kSoftCapBytes` (`paginationengine.h:17`) and states the constant is referenced, not hardcoded.
- **M2 (truncation wording)** — "the lowest-priority smallest ones" was loose against the (size-desc, id-asc) sort when sizes tie near the cap. **Fix:** §2.5 reworded to "the trailing entries in the sorted (size-desc, id-asc) order — the smallest-or-equal, lowest-priority groups."
- **M3 (test-seam parity overstated)** — ANTS-2154's real seam is the namespaced free function `mcp::setSpillDirOverride` (`src/mcpspill.cpp`), not a class member; "mirroring the pattern" implied structural parity. **Fix:** INV-12 reworded to "the same override-seam *idea*, but structurally a `RemoteControl` member — not a namespaced free function — since the cap default lives with the cache."

Dismissed (verified false): both HIGH line-cite findings (spec cites correct, reviewer miscounted); `rcComputePossibleDuplicates` "970-1025" (true close brace is `1024`); `rcNormaliseHeadline` punctuation-set under-specification (the spec *reuses* the function, exact set is not contract-relevant); `section_index` branch "3047" (line `3047` is a comment; the `if` is at `3048`). INFO: Status-line freeform (verified conforming — `docs/standards/specs.md:65-68` prescribes exactly the `spec draft, cold-eyes loops … folded (DATE)` inline form); no-TOC (genre nit, INFO only). Code-side open question carried (not a doc fix): whether `PaginationEngine::measureCutPoint` is a public symbol or a file-static helper — impact nil since bundles mode does not call it.

### Loop 5 (2026-06-24) — 2 independent cold reviewers, briefed identically

No CRITICAL, no HIGH; **no loop-1..4 finding resurfaced** (all prior fixes held). Both reviewers independently confirmed the loop-4 verdict that the dismissed HIGH line-cites were reviewer miscounts. Two findings verified and folded:

- **M1 (both reviewers) — stale number in the loop-4 log.** The loop-4 entry wrote the `bad_mode` "return `…2803`"; the `return QJsonDocument(out)` is at `remotecontrol.cpp:2802` (`2803` is the block's closing brace). A 1-line miscount recorded as "verified" — the exact verify-before-stating failure mode. **Fix:** loop-4 log corrected to "`return` at `2802`".
- **M2 (reviewer A) — `items[]` vs the ROADMAP sketch unreconciled.** The bullet sketches `{bundle_label, ids[]}`; the spec emits richer `items[]` objects (needed for the per-item refinement flags) but never said so, so a bullet-cross-checking implementer could read a mismatch. **Fix:** §2.5 gains an explicit "`items[]` vs the ROADMAP sketch" note marking the enrichment deliberate.

Dismissed as INFO (not actioned): §2.5 truncation prose is one ~120-word sentence (accurate, dense); no inline empty-set envelope example (INV-10 carries the contract); `bundle_label` example label-from-tokens vs lanes-from-union (§2.4 resolves it); `Kind: enhancement` vs the standard's `implement` example (a valid Kind, matches the ROADMAP bullet). Code-side open questions carried unchanged (measureCutPoint visibility; eager-tokenisation TTL budget) — both confirmed nil/low impact, neither a doc fix.

### Loop 6 (2026-06-24) — 2 independent cold reviewers, briefed identically

No CRITICAL, no HIGH; **no loop-1..5 finding resurfaced.** Both reviewers independently re-counted every code citation and confirmed all accurate (including the loop-5 `bad_mode` return `2802` correction). The loop has **converged to polish-only** — no structural / mechanical / architectural findings remain; only precision and test-completeness refinements, folded:

- **Trailing-strip set (§2.3)** — "strips trailing punctuation only" now names the exact chopped set (`.,;:!?` + space, `remotecontrol.cpp:947-951`) and notes `)`/`"` are not stripped, so a test author predicting tokens isn't surprised.
- **INV-4 inclusive-gate boundary** — the test list now pins both a just-below-0.60 case (keys absent) and an exactly-≥-0.60 case (keys present, score 60), locking the `>=` gate against an implementer's `>` slip.
- **Combo-guard insertion (§2.1)** — "parity with the existing guards" expanded to name the three specific new guards (`bundles`+`section`/`id`/`ids`) modelled on the three `section_index` guards, and to warn off duplicating the mode-independent `id`/`ids`×`section` guards already in the chain.
- **Lane tie-break (§2.4)** — clarified the label fallback tally tie-breaks on the *case-folded* key, distinct from the verbatim-`lanes`-union sort, so INV-11/INV-13 aren't implemented against the wrong key.

Dismissed (INFO / verified false): no-TOC and no-inline-empty-set-example (genre nits); ANTS-2154 attribution of the spill seam (the seam exists exactly as described — non-load-bearing). Code-side open questions carried (not doc fixes): confirm at implementation that the cache-populate path sets `headline_oneline` and `lanes` on every object unconditionally (loop-1 verified `headline_oneline`; `lanes` always-present is the remaining build-ordering assumption INV-1/§2.2 lean on), and that the all-bullet token sets are built once for ✅ items too.

**Loop closed at loop 6** — convergence to polish-only confirmed across two consecutive HIGH/CRITICAL-free passes (loops 5 and 6); spec marked **accepted**, ready to implement.
