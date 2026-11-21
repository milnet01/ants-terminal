# ANTS-3543 — Auto-downshift a would-be-truncated list to its lean projection instead of dropping the tail — review loop log

Record for [`../specs/ANTS-3543-auto-downshift-would-truncated-list.md`](../specs/ANTS-3543-auto-downshift-would-truncated-list.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1** (2026-07-19) — 2 lanes (engine+roadmap_query; workspace_search+scope),
strong-model cold read, all findings verified against current source.
`CRITICAL 0 · HIGH 1 · MEDIUM 5 · LOW 4 · INFO 1` (verified 11 / unverified 0).

- **HIGH — stale `truncated:true` on a fitting downshift (workspace_search).**
  `capJsonArrayToBytes` (remotecontrol.h:256) sets `truncated:true` on a trim and
  clears nothing on a fit; `out["truncated"]` also carries the scan-cutoff
  meaning from remotecontrol.cpp:10785. The § 2.3 draft removed only
  `results_dropped`, so a fully-returned lean list shipped a false
  `truncated:true`. *Fixed:* § 2.3 now snapshots the pre-cap scan-`truncated` and
  recomputes `truncated = scanTruncated || (lean re-cap dropped)`; INV-8 asserts
  it, incl. the scan-cutoff-survives case; § 2.4 documents the per-verb signal.
- **MEDIUM — downshift gate `limit < 0` mischaracterised the engine.**
  `paginationengine.cpp:66` auto-picks on `limit <= 0`, so `limit==0` is auto,
  not a page size. *Fixed:* gate + INV-2 changed to `limit <= 0`; INV-7 notes the
  verb rejects `limit==0` with `bad_args` before the engine.
- **MEDIUM — projector wired to the wrong set of call sites.** § 2.2 lumped the
  `section_index` `sections[]` branch with the bullet sites; section descriptors
  have no lean form. *Fixed:* projector wires the two bullet sites only;
  `section_index` excluded (like find_sources); INV-5 covers both bullet sites;
  the exactly-5-calls scrape noted as preserved.
- **MEDIUM — INV-3 "strictly more rows" overclaim.** Empty-body rollup bullets
  make lean≈fat. *Fixed:* "no fewer (strictly more when dropped fields carried
  bytes)"; test fixture uses body-bearing rows.
- **MEDIUM — INV-8/INV-10 had no executable test.** *Fixed:* § 6 adds a pure
  `capJsonArrayToBytes` truncated-honesty test (would have caught the HIGH).
- **MEDIUM — § 7 omitted the in-code schema descriptions.** roadmap_query
  (claudeintegration.cpp:2385) enumerates the envelope fields. *Fixed:* § 7 adds
  a same-commit bullet to add `downshifted` there.
- **LOW ×4 / INFO ×1** — header citations pointed at call sites not definitions
  (`rcApplyHeadlineOnly` def remotecontrol.cpp:431; `capJsonArrayToBytes` def
  remotecontrol.h:256); "idempotent no-ops" reworded to "block skipped"; § 2.4
  `next_offset`/`results_dropped` verb-conflation split; Layman honesty qualifier;
  § 5 changelog "near-free" softened to name the projector caveat. *All fixed.*

**Loop 2** (2026-07-19) — same 2 lanes, cold re-read (no fix-briefing); both
lanes independently confirmed the loop-1 HIGH is fixed (4-case truth table
holds). All new findings verified against current source.
`CRITICAL 0 · HIGH 0 · MEDIUM 5 · LOW 5 · INFO 2` (verified 12 / unverified 0).

- **MEDIUM — bundles-mode truncation uncovered.** `buildRoadmapBundlesEnvelope`
  (remotecontrol.cpp:3391) drops whole trailing `bundles[]` — a third
  roadmap_query truncation path, same silent-tail shape, neither wired nor
  scoped. *Fixed:* § 5 adds an explicit out-of-scope bullet (bundles have no flat
  lean form; deferred to a follow-up).
- **MEDIUM — INV-2 `limit==0` test contradicted the engine contract.**
  `paginationengine.h:50` declares `0` invalid (rejected upstream). *Fixed:*
  INV-2 drops the `limit==0` assertion and explains `<= 0` is chosen only to
  match the engine's auto-pick branch defensively (`< 0`/`<= 0` coincide since 0
  never reaches it).
- **MEDIUM — § 2.3 dropped the `bytes_cap_clamped` echo.** The inline cap it
  replaced sets it (remotecontrol.cpp:10873). *Fixed:* the extracted helper
  captures `capClamped` from both cap calls and re-emits it.
- **MEDIUM ×2 — the recompute + `also_at` had no executable test.** *Fixed:*
  § 2.3 extracts the workspace_search downshift into a **pure `static inline`
  header member** `RemoteControl::downshiftMatches` (sibling of the already
  unit-tested `capJsonArrayToBytes`; takes the lean projector as a callback so it
  doesn't link the `.cpp`-local `rcApplyHeadlineOnly`), so § 6 now pure-tests
  INV-8/9/10 across the 4-case truth table incl. `also_at` + `bytes_cap_clamped`,
  socket-free — the executable lock on the loop-1 hazard.
- **LOW ×5 / INFO ×2** — § 2.1 changelog "reach through hook" softened to
  "eligible (deferred)"; INV-3 verb-agnostic ("projector removed bytes", not
  "context"); INV-5/6/7/11 test surfaces aligned to the pure+scrape split and
  emit-site counts made precise; full-file site named (`:5230`); `INV-5b`
  qualified as ANTS-1876; § 4 RAM reworded for Qt copy-on-write; § 7 schema note
  covers the cleared drop-signals + widened `headline_only` meaning. *All fixed.*

**Loop 3** (2026-07-19) — same 2 lanes, cold re-read (no fix-briefing).
**Zero substantive findings** — both lanes confirmed the mechanism sound, the
loop-1 hazard closed, and `RemoteControl::downshiftMatches` buildable +
pure-testable (verified the `rc_read_tool_byte_cap` test calls its sibling
`capJsonArrayToBytes` the same socket-free way). All findings polish.
`CRITICAL 0 · HIGH 0 · MEDIUM 2 · LOW 2 · INFO 4` (verified 8 / unverified 0).

- INV-5's *Test:* named a socket-level "behavioural check" that § 6 disclaims →
  reworded to the pure INV-1 empty-projector case + the two-site scrape.
- The § 6 workspace_search scrape locked routing but not projector *identity* (an
  empty/wrong projector would blank every row uncaught) → now asserts
  `rcApplyHeadlineOnly` is the projector passed, mirroring INV-5.
- INV-8's *Test:* enumerated 2 of the 4 truth-table cases → now names all four.
- Polish: § 4 "value parameter" → const-ref; `<functional>` "transitively
  available" → "add explicitly"; redundant second `capClamped` simplified (same
  `maxBytes`); `buildRoadmapBundlesEnvelope` citation `:3390`→`:3391`; §9 loop-2
  "free function" → "static-inline header member"; noted `count_only`/`files_only`
  return before the cap (nothing to downshift).

**Converged at loop 3** — a polish-only pass with no structural / mechanical /
architectural finding remaining. Ready for sign-off.
