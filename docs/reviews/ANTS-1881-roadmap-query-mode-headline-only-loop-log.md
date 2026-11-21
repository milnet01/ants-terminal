# ANTS-1881 — `roadmap_query` `mode:"headline_only"` — review loop log

Record for [`../specs/ANTS-1881-roadmap-query-mode-headline-only.md`](../specs/ANTS-1881-roadmap-query-mode-headline-only.md) § 9. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-05-26).** Reviewer flagged 3 HIGH (heading drift from
  specs.md, missing `*Test:*` clauses, INV-2 rollup-key ambiguity),
  5 MEDIUM (link convention `[[…]]` undocumented, etag wiring lacked
  source cite, line-range pin drift risk, INV-5 vacuous, test-anchor
  count mismatch), 5 LOW (~10× claim provenance, `Bullet::status`
  typing, rollup gate, `PaginationEngine` naming, Status-line format).
  All verified against `docs/standards/specs.md`, `src/remotecontrol.cpp`,
  `src/claudeintegration.cpp`, `docs/specs/ANTS-1437-roadmap-query-section-index-mode.md`. Fixes:
  renumber sections per specs.md §3; rewrite INV-2 to govern key-set
  with empty-value permission; add `*Test:*` clauses to every INV;
  convert INV-5 from vacuous to "combinator coverage" positive claim;
  switch link convention from `[[…]]` to plain `ANTS-NNNN`; add
  `claudeintegration.cpp:7371` cite to §2.5; replace `line 1629–1643`
  pin with function-name pin; add `PaginationEngine` name to §2.3;
  Status line normalized to `spec draft (2026-05-26)`.
- **Loop 2 (2026-05-26).** Reviewer flagged 1 HIGH (`fields=` row
  lacked `isFieldProjectionTool` cite — corpus consistency rule),
  3 MEDIUM (no INV locking the diagnostic carve-out asserted in §2.4;
  line-number pin reintroduced at `claudeintegration.cpp:7371` despite
  loop-1 fix; INV-1 present-tense audit vs post-fix framing). All
  verified against `src/mcpprojection.cpp` (`isFieldProjectionTool`
  exists), `src/claudeintegration.cpp` (line drift risk confirmed),
  spec body. Fixes: add `mcp::isFieldProjectionTool` /
  `makeFieldsProp` cite to the `fields=` row; replace
  `claudeintegration.cpp:7371` with function-name pin only; reframe
  INV-1 with "After this change, …"; add INV-8 locking the
  `legacy_format_sections` / `legacy_format:true` / `duplicate_ids`
  diagnostic omission with a fixture-based test surface; update §4
  fixture description + anchor count (8).
- **Loop 3 (2026-05-26).** Reviewer flagged 1 HIGH
  (INV-8 over-carve: `duplicate_ids[]` is a roadmap-integrity signal
  that benefits headline_only callers and is payload-cheap; the
  "wire-cost optimisation" justification didn't cover it),
  2 MEDIUM (INV-7 source-grep too loose — would pass on the
  impl-side allow-set match alone; §2.5 elided the
  `isEtagSupportedTool` precondition that fires `applyEtagPattern`),
  2 LOW (Status line lacked the cold-eyes-loops cumulative marker
  per specs.md §5.7; missing **Layman:** line per specs.md §5.2).
  All verified against `src/remotecontrol.cpp` (`duplicate_ids[]`
  emitted on every bullets-mode branch — 2064/2265/2416/2526),
  `src/claudeintegration.cpp:7332` (`roadmap_query` in
  `isEtagSupportedTool`), `docs/standards/specs.md` §5.2 + §5.7.
  Fixes: split INV-8 — keep `duplicate_ids[]`, carve out only
  `legacy_format_sections[]` + per-section `legacy_format:true`;
  anchor INV-7's grep to `src/claudeintegration.cpp` + the `mode`
  enum context (impl-side allow-set match alone fails closed);
  add half-sentence to §2.5 naming `isEtagSupportedTool` as the
  precondition; update Status line to record the three folded loops;
  add **Layman:** line under the header block; also update §2.4
  prose to match the new split-carve-out story.
- **Loop 4 (2026-05-26).** Reviewer flagged 0 HIGH, 2 MEDIUM
  (INV-7 grep recipe still missing the *positive* anchor — the
  `roadmapTool["name"] = "roadmap_query"` bracket and the
  `modeEnum.append(…)` context line; §7 named only one descriptor
  touchpoint when there are actually two — per-arg
  `modeProp["description"]` AND top-level
  `roadmapTool["description"]` mode roster), 4 LOW/INFO (function-
  level anchor cite for `applyEtagPattern` ordering; ~50 LoC
  schema-update budget under-counts; ROADMAP-only cross-refs OK;
  bullet-level `fields=` projection not explicitly stated as
  unsupported). All verified against
  `src/claudeintegration.cpp:1805-2008` (both descriptor blocks
  exist; the top-level enumerates modes at lines 1834-1842, the
  per-arg one defines `modeEnum` separately). Fixes: tighten INV-7
  grep recipe with `roadmapTool["name"] = "roadmap_query"`
  positive bracket and a `modeEnum.append(…)` context anchor; split
  §7 into three numbered touchpoints (enum + per-arg description +
  top-level description roster); extend INV-2 with explicit
  `fields=` exclusion plus a second `*Test:*` anchor
  (`Inv2FieldsDoesNotNarrowBullets`); update Status line + cold-eyes
  log.
- **Loop 5 (2026-05-26).** Reviewer flagged 1 CRITICAL
  (INV-8 / §2.4 misattribute `legacy_format_sections[]` +
  `legacy_format:true` to bullets-mode — they are emitted only
  inside the `mode == "section_index"` branch of `cmdRoadmapQuery`
  at `src/remotecontrol.cpp:2019/2050`), 1 HIGH (§2.2/INV-2
  contradiction on narrator entries: §2.3 says narrators have
  non-empty `headline_oneline` but INV-2's "empty-string values
  permitted (rollup / narrator)" implies both empty), 3 MEDIUM
  (INV-3 should state positional vs set-based equality; INV-7
  reintroduced `(currently :1806)` line-number hedge despite
  loop-1's ban; §7's top-level-descriptor extension may produce a
  wall of text — should be a short selection hint, not full payload
  shape), 6 LOW/INFO (clarification of bullet-level `fields=`
  exclusion rationale, test-anchor naming consistency, etc.). All
  verified against `src/remotecontrol.cpp:1858-2066` (legacy_format
  emitters live inside section_index branch only). Fixes: rewrite
  INV-8 to a positive top-level-diagnostic-parity claim
  (`duplicate_ids[]` byte-identical between modes; legacy_format
  diagnostics naturally absent because they're section_index-only);
  fix INV-2 to distinguish rollup-empty vs narrator-non-empty
  `headline_oneline`, add separate test anchors for each; add
  "positional equality" to INV-3; drop `(currently :1806)` from
  INV-7; rewrite §7 prose with explicit "Three touchpoints" framing
  and separate technical vs selection-hint wording for the per-arg
  vs top-level descriptions; add `fields=` exclusion rationale to
  INV-2 body.
- **Loop 6 (2026-05-26).** Reviewer flagged 0 CRITICAL, 3 HIGH
  (INV-5 / §2.3 silently miss the `id=` selector emission path at
  `src/remotecontrol.cpp:2375-2419` — that branch builds its own
  envelope from a scanned `matches[]` array without going through
  the main bullets-mode emit loop, so projection must be applied
  there too; Cold-eyes log lists Loop 5 before Loop 4 — out of
  chronological order; loop-5 log entry references a "Two
  touchpoints" §7 state no longer in the doc), 4 MEDIUM (INV-3
  positional equality should ground in shared cache iteration
  order; INV-8 wording obscures the shared-lineage rationale;
  INV-4 single test anchor name is ambiguous between sub-claims (a)
  and (b); §5 LoC budget under-counts the descriptor work),
  4 LOW/INFO (test-anchor name mixing; quote-escape risk in §7
  descriptor text; etc.). All verified against
  `src/remotecontrol.cpp:2375-2419` (id-branch returns before the
  main emit loop), spec body. Fixes: add the id-branch as a second
  projection surface in §2.2 + add explicit row coverage in §2.3 +
  add `Inv5IdSelectorProjected` test anchor; reorder Cold-eyes log
  to chronological (loop 4 → loop 5 → loop 6); rewrite loop-5
  entry to match the spec's actual state; ground INV-3 positional
  equality in shared cache; reword INV-8 to "diagnostic parity by
  shared lineage" with explicit section_index-only carve-out;
  split INV-4 test anchors into `Inv4SameMode*` + `Inv4CrossMode*`;
  bump §5 LoC estimate from ~230 to ~280 with per-touchpoint
  breakdown.
- **Loop 7 (2026-05-26).** Reviewer flagged 0 CRITICAL, 1 HIGH
  (`mode` echo behaviour ambiguous: section_index emits
  unconditionally at `src/remotecontrol.cpp:2026`, bullets-mode id
  branch emits conditionally on `hasModeArg` at `:2409`; spec didn't
  say which precedent headline_only follows), 3 MEDIUM (INV-2
  narrator non-empty claim not grounded in a named predicate;
  §2.4 phrasing "projects bullets" misreads as "is a bullets
  emitter"; INV-7 grep recipe vulnerable to false-pass against the
  description-block literal `"headline_only"` once §7 lands),
  4 LOW/INFO (LoC budget consistent now; status emoji typing fine;
  isFieldProjectionTool cite sufficient; test-anchor `Inv4a*` /
  `Inv4b*` lowercase suffix inconsistent with full-word suffix
  style). All verified against `src/remotecontrol.cpp` (1721
  `isRollupBullet`, 1730 `isNarratorBullet`, 2026 + 2409 mode-echo
  precedents), `src/claudeintegration.cpp:1810+` (descriptor block
  contains "headline_only" literal post-fix). Fixes: state in §2.4
  that headline_only follows the bullets-mode `hasModeArg` echo
  gate (rationale: bullets-projection variant → bullets-mode
  default-call byte-identity); ground INV-2 narrator clause in
  `isNarratorBullet` predicate cite; reword §2.4 "projects bullets"
  → "runs through the bullets-emission code paths" to avoid
  misread; tighten INV-7 to require literal inside a
  `modeEnum.append(...)` call (not just a context line near one);
  rename test anchors `Inv4a*` → `Inv4SameMode*`, `Inv4b*` →
  `Inv4CrossMode*` for style consistency.
- **Loop 8 (2026-05-26).** Reviewer flagged 0 CRITICAL, 2 HIGH
  (INV-8 cites a line range `:1858-2058` despite loop-1's ban on
  line-number pins — should be function-name pin
  (`cmdRoadmapQuery`); INV-2 narrator clause cites
  `isNarratorBullet` predicate then says "narrator's defining
  condition is …, so `rcHeadlineOneline(b.headline)` is non-empty"
  — the predicate-cite attaches to a derived fact, should attach
  to the field it actually tests then derive the rest),
  3 MEDIUM (INV-7 test recipe should ship the actual `grep -nP`
  pattern not just "source-grep"; §2.3 id-row envelope list omits
  `mode` echo — should include it conditional on `hasModeArg`;
  source-column names in §2.2 mix parseBullets struct fields with
  JSON keys), 5 LOW/INFO (Cold-eyes log Loop 7 before Loop 6 —
  same ordering regression loop-6 already fixed once; descriptor
  prose still escapes quotes that won't be in the live string;
  etc.). All verified against `src/remotecontrol.cpp:1730`
  (`isNarratorBullet` tests `headline` non-emptiness, not
  `headline_oneline`), spec body. Fixes: replace `:1858-2058` with
  function-name pin (`cmdRoadmapQuery`); reword INV-2 narrator
  clause so predicate cite attaches to `headline` (the field it
  tests), then derive `headline_oneline` non-emptiness; tighten
  INV-7 recipe with the actual `grep -nP` command; add `mode`
  (conditional) to §2.3 id-row envelope list; reorder Cold-eyes
  log so Loop 6 precedes Loop 7.
- **Loop 9 (2026-08-26).** 3 lanes, cold, genre pinned `spec`; new run,
  trigger ANTS-4703 part 2. Q1 5 · Q2 2 · Q3 1 · Q4 2 — **ten verified,
  ten fixed, none dismissed.** Six lane open questions resolved clean.
  **All three lanes independently found the same three Q1s**, the
  strongest agreement this document has produced. The largest is that
  `cmdRoadmapQuery` was moved to `src/remotecontrol_roadmap_query.cpp`
  and every body citation still named `src/remotecontrol.cpp` — INV-7's
  exclusion rule among them, so its discriminator named a file that no
  longer holds the allow-set. Line-range pins were dropped rather than
  re-derived; loop 3 made the same call. Second, §2.3's `fields=` row
  cited `mcp::isFieldProjectionTool`, which ANTS-4524 **deleted** —
  there is no allowlist and every verb honours `fields=`. Third, INV-5's
  enumeration was stale in both directions: it named `fields` and
  `etag_match`, which the function does not read, and omitted a dozen it
  does. **The most dangerous was found by one lane only** — INV-1 and
  §2.1 asserted a three-value allow-set against the live `kModes`, and
  §2.1 carried it as a buildable `!=` chain, so an implementer
  re-applying the snippet would have rejected `bundles` and `report`.
  **INV-5 was re-derived rather than widened**: it now claims every
  argument the verb reads is either given behaviour in §2.3 or named
  there as having no effect, which is true, carries no enumeration to go
  stale, and is the first form of it a test could enforce. §2.3 gained
  rows for `ids`, the `query` family and `max_body_bytes`, and one row
  naming the other-mode arguments that are read and ignored — the silent
  default the old INV-5 denied existed. **Two test clauses were made
  honest rather than repaired**: `Inv5CombinatorCoverageSourceGrep`
  asserts only that the projection helper is named, and
  `Inv2FieldsDoesNotNarrowBullets` only that the mode value appears in
  the sources; neither can falsify its clause, and the code-side repair
  is out of a docs gate's scope. **Two lane findings were corrected
  before landing.** "Four emission surfaces" is three selector shapes —
  the list path projects at two sites, plus the `id` and `ids` branches
  — and `ids` is the one the document never named. And INV-2's `fields=`
  clause is *true*; the defect was its test, not its premise. **Packet
  defect, disclosed:** the orchestrator's source window was cut
  mid-sentence, so §2.2, §2.4, §2.6, INV-6 and INV-7 went unreviewed on
  their behavioural half this loop. **Collateral filed, not fixed:**
  sibling specs still cite the pre-split file and the deleted predicate;
  each has its own gate.
- **Loop 10 (2026-08-26).** 3 lanes, cold, packet rebuilt whole from disk
  with loop 9's truncated window repaired and windows added for the mode
  allow-set, the bullet predicates, the `id`/`ids` branches, the shared
  projection helpers and the `tools/list` descriptor. Q1 1 · Q2 4 · Q3 0
  · Q4 1 — **six verified, six fixed, none dismissed.** Three lane open
  questions resolved clean. **Cap reached (2 for a spec); the run files
  its tail and exits.**
  **Loop 9's INV-5 rewrite HELD** — all three lanes independently walked
  the verb's whole read-set against §2.3 and confirmed the claim, which
  is the check the old enumeration could never pass.
  **The run's most valuable output is a CODE defect, filed as ANTS-4712,
  not fixed here.** In the `ids` branch the projection runs first and
  ANTS-4400's `input_index` loop then writes a fifth key onto each
  bullet, so `mode:"headline_only" + ids:[…]` breaks INV-2's four-key
  contract. Nothing caught it because INV-5's only behavioural anchor
  exercises the singular `id`, and the source-grep anchor is satisfied by
  the helper's own definition — so it vouches for nothing behavioural.
  Both facts are now stated rather than implied.
  **Two findings landed on text loop 9 wrote**, which is the expected
  shape: the `ids` bullet asserted `input_index` while the four-key rule
  three lines below denied it, and the grouped ignore-row swept in
  `section_etag_match`, which is read on the `section=` path this mode
  uses and is honoured. Two more landed where loop 9's edits made
  pre-existing text contradictory (§5's touchpoint list, INV-8's path
  enumeration). Two were untouched by this run: INV-7 restated the
  descriptor enum's membership as three values where it ships five, and
  INV-3's parity claim cannot hold wherever INV-6's byte-measured
  truncation does — a parity test over emitted `bullets[]` would red
  against correct code, so INV-3 is now scoped to the pre-pagination set.
  **Cap character: calm rather than oscillating.** Roughly half the loop
  landed on this run's own text, but in *kind* — a row's reason, an
  enumeration — with none of loop 9's decisions reversed, and the two
  pre-existing findings were real. At 579 lines the document is well
  inside the size range where a split would be indicated, so the cap is
  the normal spec exit and not evidence against the document.
  **Deferred tail:** ANTS-4712 (the `ids` five-key defect and the two
  missing test anchors) and ANTS-4711 (sibling specs citing the pre-split
  file and the deleted `isFieldProjectionTool`). Neither is a docs fix to
  this document.

- **Loop 11 (2026-08-27).** 3 lanes, cold, genre pinned `spec`; packet
  rebuilt whole from disk with windows on the three emission surfaces, the
  shared projection helper and the `mode` schema text. Q1 3 · Q2 2 · Q3 0
  · Q4 1 — **six verified, six fixed, none dismissed.** Trigger: ANTS-4712
  widened INV-2 to name the `ids` branch's `input_index` exemption, which
  loop 10 had filed as its deferred tail. **All three lanes independently
  found the same four defects** — the strongest agreement this document's
  log records. **Three were this loop's own collateral, and all three are
  the same failure**: INV-2 was widened and the passages it contradicts
  were left standing — §2.2 item 3 still called the exemption an open
  question, §2.3's `ids` row still asserted parity with the `id` branch,
  and INV-5 still said no anchor exercises the `ids` branch after INV-2
  named one. Delete-first would have caught all three. **The most
  consequential was PRE-EXISTING and older than this run**: §4 prescribed
  a harness that dispatches `RemoteControl::cmdRoadmapQuery` in-process,
  which cannot link — the feature bundles do not link the verb, and both
  named suites are source scrapes. An implementer building §4 as written
  gets an unlinkable bundle. Its consequence is now stated rather than
  implied: no anchor in that suite observes a key set, a 304, a page slice
  or a `duplicate_ids` payload, and §4 says so, because a scrape and a
  behavioural test read alike in a green run. **The Q4 came from the same
  root** — INV-4, INV-5 and INV-8 each prescribed an assertion requiring a
  live response; each is restated as the scrape actually shipped. **The
  sixth was the orchestrator's, not a lane's**, and invisible to them
  because the packet carried no window on the test file: INV-4 named
  `Inv4SameModeEtagSameModeShortCircuit` and
  `Inv4CrossModeEtagCrossModeNoShortCircuit`, and neither exists — the
  suite spells them `Inv4SameModeShortCircuit` and
  `Inv4CrossModeNoShortCircuit`, as the feature spec had all along.
  Verified against `ctest -N`. **A fix's own added claim was caught by 4a
  step 3**: it called the INV-4 anchors scrapes over the handler, and they
  scrape the dispatcher, where the etag is applied. Collateral, out of
  scope and corrected there:
  `tests/features/roadmap_query_headline_only/spec.md`'s INV-5 row counted
  two emission surfaces where §2.2 names three. Three lane open questions
  resolved clean and are therefore not in the tally — `applyEtagPattern`
  does wrap after the handler returns (`src/claudeintegration.cpp`), §7's
  enum framing restates nothing, and INV-5's read-set claim was correctly
  left unfiled for want of a window.

- **Loop 12 (2026-08-27).** 3 lanes, cold, same brief; packet rebuilt
  whole from disk and EXTENDED with the two test suites' headers, the
  shipped INV-4 and INV-8 anchors, and the `ctest -N` name list — the gap
  that hid loop 11's sixth defect from every lane. Q1 1 · Q2 6 · Q3 0
  · Q4 1 — **eight verified, eight fixed, one dismissed.** **Cap reached
  (2 for a spec); the run files its tail and exits.**
  **Half the verified findings landed on text loop 11 wrote** — four of
  eight, each anchor checked against loop 11's ledger rather than recall.
  That is a high share, so this is the oscillating cap rather than the
  calm one: loop 11 widened INV-2 and left its neighbours standing, loop
  12 repaired those and introduced one of its own. **Do not gate this
  document again as it stands.** Route it to implementation, which
  exercises the contract against real code and is the better third
  reviewer; an authoring edit that changes direction re-arms the gate
  normally.
  **The three PRE-EXISTING findings are why the loop was still worth
  running, and one is the most dangerous defect in the document.** §2.2
  said "three selector shapes" and §5 costed "three branches", while the
  list path is TWO separate emit loops — four projection call sites, not
  three. An implementer wiring one ships `headline_only` still emitting
  fat bullets on whichever of the section-scoped or full-file call they
  missed, and INV-5's coverage grep cannot catch it because the helper's
  own definition satisfies it. Found by one lane; a second reached the
  same inconsistency from the opposite direction and **the orchestrator
  dismissed that one in error**, on the reasoning that §2.2 item 1 already
  covered both paths. It did not — it said "the filtered-array loop",
  singular. The dismissal is withdrawn here rather than by editing loop
  11's row. **Second:** §4 stated red-first as a blanket exit condition,
  while INV-4's and INV-8's anchors pin constructs that predate this
  change and are green in both states — an implementer holding that
  condition edits a correct test until it fails. **Third:** §2.3 gave
  `status` and `offset`/`limit` unconditional "Honoured" rows, which the
  `id` / `ids` selectors bypass by design (ANTS-1726), so building the
  table's contract literally would break that bypass.
  **The four collateral findings were loop 11's own fixes**: the INV-8
  test sentence claimed a positional assertion the shipped anchor does
  not make, §4 said every anchor scrapes the `remotecontrol` sources
  when INV-4 and INV-7 scrape `claudeintegration.cpp`, §5 still costed a
  `QTemporaryDir` fixture loop 11 had removed from §4, and INV-3 was left
  without the "unobserved" disclosure loop 11 gave INV-4, INV-5 and
  INV-8. **A fix of this loop added its own false claim and 4a step 3
  caught it before it landed:** it attributed the etag-allowlist fact to
  INV-4, which does not state it; §2.5 does.
  **One dismissed:** §2.3's unconditional `headline_oneline:""` for
  rollups reads as stricter than the helper's conditional comment, but
  `isRollupBullet` requires an empty `headline`, so the condition is
  always true and no build changes. Two lanes and the orchestrator agreed
  independently.
  **One lane open question became a finding rather than a deferral**:
  `applyQueryFilter` runs at two sites, both list loops, so §2.3's `query`
  row had the same shape as the `status` gap — but the code REFUSES
  `bad_mode_combo` on `query` + `id`/`ids` rather than bypassing, and the
  row now states the contrast so neither behaviour is generalised to the
  other. **Deferred tail: none** — every verified finding was fixed. Two
  open questions remain unsettled for want of a window and are recorded
  rather than guessed: §2.3's `section_etag_match` cross-mode claim, and
  whether the list path's `mode` echo is `hasModeArg`-gated as the `id`
  branch's is.
