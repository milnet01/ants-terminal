# ANTS-2079 — Trim load-bearing MCP tool-description blobs; serve per-op detail via `tool_info` — review loop log

Record for [`../specs/ANTS-2079-trim-load-bearing-mcp-tool.md`](../specs/ANTS-2079-trim-load-bearing-mcp-tool.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

### Loop 1 (2026-06-16) — two independent cold reviewers, all findings verified + folded

- **HIGH — byte-figure conflict + wrong metric.** §1 cited ~6.8/5.5 KB
  vs the journal's ~1.5/2.1 KB, and conflated source bytes with wire
  bytes. **Verified** by re-measuring every literal: the relevant metric
  is *wire-content* bytes (decoded string shipped in `tools/list`), real
  sizes ~4561→~1872 B. Fixed: §1 now defines the metric, gives measured
  values + anchor lines + a reproduction recipe, and marks the journal
  figures stale/superseded.
- **HIGH — scope threshold used the wrong metric.** The 2500 B bar (on
  source bytes) would, on wire bytes, wrongly drop 3 of the 7 named
  tools. Fixed: §2.5 bar is now ≥ ~1800 B wire-content, landing in the
  clear ~260 B gap above `cold_eyes_brief` (~1609 B); the seven-tool set
  is unchanged but now honestly derived.
- **HIGH — 700/800/900 B ceiling inconsistency.** §2.1/§2.4/§2.5/INV-5
  gave three different ceilings. Fixed: one authoring target (≤ 700 B)
  + one test ceiling (≤ 800 B = 700 + ≤ ~65 B pointer); INV-5 + §2.1 +
  §2.5 reconciled with the arithmetic stated.
- **HIGH — INV-3 untestable as written.** "union vs the pre-trim
  description" can't run (the old literal is deleted in the same commit).
  Fixed: INV-3 now tests against a per-tool **anchor token list** in the
  fixture, explicitly noting the pre-trim literal is unrecoverable.
- **MEDIUM — "mirroring `selection_hint`" misleading.** `selection_hint`
  is set unconditionally; copying that verbatim would emit `detail:null`
  for ~66 tools and break INV-2. **Verified** against code (@ 8631,
  unguarded). Fixed: §2.3 now contrasts the two and states the
  conditional set is deliberate.
- **MEDIUM — `test_audit_partition` `detail`-arg collision.**
  **Verified** (`props["detail"] = p`, claudeintegration.cpp:4547). The
  descriptor field and the input arg never alias (different JSON
  objects), but an auditor/INV-3 author could confuse them. Fixed: §2.5
  collision note added.
- **MEDIUM — strip-loop edge cases + JSON escaping undefined.** Fixed:
  §2.1 precondition (non-empty `description` AND `detail` per in-scope
  tool); §2.2 notes tool names are `[a-z_]+`, so the pointer
  interpolation is escape-safe.
- **MEDIUM — strip-loop perf not budgeted.** Fixed: §4 now quantifies
  (~7 concatenations/call; `tools/list` is not a hot path).
- **MEDIUM — Cold-eyes loop log + Layman line absent** (specs.md §5.2 /
  §5.7, global rule 14). Fixed: this section added; `**Layman:**` line
  added to the header.
- **LOW — line-citation drift / "builder" wording / INV-1..7 phrasing.**
  **Verified** against source (`description` echo @ 8626; `tool_info`
  match `else` @ 8623; `roadmap_query` name @ 1878 — a *literal*, not a
  "builder"; `wantLite` @ 8164 / branch @ 8174). Fixed throughout.
- Confirmed sound (no change): the snapshot-then-mutate copy-on-write
  argument (Qt `QJsonArray` value semantics); no other consumer reads a
  descriptor `detail` key, so the strip can't break
  `categoryOf`/`kindForName`/lite-shape/`tokenCostFor`; spec conforms to
  the `specs.md` required-section skeleton.

### Loop 2 (2026-06-16) — two fresh cold reviewers, no fix briefing; loop-1 fixes confirmed held

Both reviewers independently re-verified the loop-1 changes (numbers now
agree across §1/§2.5/INV-5; metric used consistently; citations exact;
copy-on-write argument sound; conditional `detail` set correct). No
CRITICAL. New findings, all verified + folded:

- **HIGH — INV-6 over-credited the wrong guard.** It cited
  `mcp_tool_info_verb` INV-12 for the `[<kind>]`-prefix runtime check, but
  **verified** (test_mcp_tool_info_verb.cpp:232-244) INV-12 only
  source-scrapes the `categoryOf` handler — it never checks a tool's
  runtime prefix; a dropped prefix would ship green. Fixed: INV-6 now
  rests on `mcp_tool_prefix_tags` INV-3 + the new test's own
  `startsWith("[")` assertion, with INV-12 explicitly demoted to a non-guard.
- **HIGH — INV-7 didn't fail fast on reversed snapshot/strip order.**
  Fixed: INV-7's test now also asserts `tool_info`'s `detail` is non-empty,
  so snapshotting after the strip fails at INV-7, not only at INV-2.
- **MEDIUM — INV-3 anchor fixture lacked a concrete, reproducible
  source.** Fixed: named the fixture path
  (`tests/features/mcp_tool_detail_field/anchors/<tool>.txt`) and pinned
  capture to `git show HEAD:src/claudeintegration.cpp` (mechanical, not
  hand-transcribed).
- **MEDIUM — byte-measurement not reproducible from the spec.** Fixed: §1
  now inlines the exact per-tool measurement command.
- **MEDIUM — no proof the densest tool fits the ceiling.** **Verified** by
  drafting + measuring `roadmap_log`'s short form: 248 B essentials + 55 B
  pointer = 303 B, well under 700/800 B. Fixed: §2.1 carries the worked,
  measured example.
- **MEDIUM — test bundle name unconfirmed.** **Verified**: the bundle is
  `test_claude` (`ants_add_gui_bundle`, CMakeLists.txt:1007). Fixed: §6
  now cites the macro + line.
- **LOW — INV-5 pointer length / §2.1 illustrative comment / §5 follow-up
  id.** Fixed: pointer template length stated (~44 B + name); §2.1 comment
  labelled illustrative + measured; §5 notes the follow-up id is allocated
  on ship.
- Reviewer open questions were code-side implementation checks (lite-shape
  `summaryFor` fallback after trim; a final grep for other `detail`
  consumers) — recorded for the implementer, not doc fixes.

### Loop 3 (2026-06-16) — one fresh cold reviewer; convergence confirmed

Zero CRITICAL, **zero HIGH** (loop 1 had 3 HIGH, loop 2 had 2 HIGH — clean
downward trend). The reviewer re-verified every code citation exact and
confirmed the loop-2 fixes held (INV-12 demotion honest; INV-3
anchor-from-`git show` resolves the untestability; numbers reconcile). 3
MEDIUM + 2 LOW, all refinements, all verified + folded:

- **MEDIUM — INV-1 lite clause vacuous.** **Verified** (claudeintegration.cpp
  ~8200-8211): the lite shape is `{name,summary,kind}`-only, so it can
  never carry `detail` regardless of this change — "neither full nor lite"
  implied a non-existent risk. Fixed: INV-1 now scopes to the full shape
  and notes the lite shape is structurally safe.
- **MEDIUM — lite `summaryFor` could leak the pointer.** **Verified +
  closed**: all seven in-scope tools set a non-empty `selection_hint`
  (roadmap_query @1982, the other six @2399/6891/5783/7471/2745/5497), so
  `summaryFor` never falls back to the pointer-bearing `description`.
  Fixed: §2.2 records this; the loop-2 "open question" is resolved, not
  deferred.
- **MEDIUM — dangling "HINT-1" reference.** **Verified**: the unguarded
  `selection_hint` set is attributed to ANTS-1453 in code (@8628-8630),
  not "HINT-1". Fixed: §2.3 now cites ANTS-1453.
- **LOW — "~66" magic number / §4 "QJsonValue references" wording.**
  Fixed: "~66 = 73 registered − 7"; §4 reworded to "shares storage
  copy-on-write".

Remaining reviewer open question (a whole-`src/` grep for any other
descriptor-`detail` reader) is an implementer pre-flight check, not a doc
gap — `props["detail"]` (the unrelated `test_audit_partition` arg) and the
new field are the only two in `claudeintegration.cpp`.

**Sign-off.** Three loops, converging cleanly (3→2→0 HIGH), every finding
across all severities verified against live source and folded; loop 3
found no CRITICAL/HIGH and only minor polish. Per the standing
review-loop standard, the spec is **signed off for implementation**.
- **doc-fix (2026-11-13, ANTS-5559; no reviewer dispatched)** — INV-8's test clause named `ctest` as a command with no result. It now reads as what it is, a statement that the five feature tests pass.
