# ANTS-1397 — `test_audit_*` MCP trio (skill-displacement migration) — review loop log

Record for [`../specs/ANTS-1397-test-audit-mcp-trio-skill.md`](../specs/ANTS-1397-test-audit-mcp-trio-skill.md) § 10. Cold-eyes loop log.

## Cold-eyes loop log

**Loop 1 (2026-05-17):** v1 raised 5 H, 5 M, 5 L. v2 folded:
fold-in delegation, regex feasibility, deterministic token, lock,
Optional contract, pagination, dimension injection, no codegen,
test plan, spec polish.

**Loop 2 (2026-05-17):** three lanes. v3 folded: namespace
PascalCase, InFlightRegistry (later inlined v4), per-message cap
(later structural via type, v4), `truncated`/`samples_truncated`
split, PathValidation across all paths, thread placement, reports_dir
anchor, pre-pass redaction, token check before cache, fold-in
atomicity, lib placement rationale, test bundle named,
`RoadmapFoldIn` direct call clarified, drift guard, mtime cache,
SHA-256 truncation note.

**Loop 3 (2026-05-17):** five lanes. v4 folded:

- **Perf H1:** fold-in now batched single read+write (not N× per
  finding). § 2.5 + INV-3 rewritten.
- **Perf H2:** pool size `max(1, nproc/8)` → `max(2, nproc/8)` in
  ANTS-1351 INV-9; pool saturation envelope + `retry_after_ms`
  documented for this spec's partition (§ 2.7).
- **Perf M4:** alternation longest-first + common-prefix-factor
  rule (INV-1).
- **Perf M8:** mtime cache disambiguated (INV-15) — bounded
  shallow recheck + 5 s rate-limit + documented deep-tree gap.
- **Sec H-A:** synth prompt fencing (INV-8 + § 2.4).
- **Sec H-B:** stale-slot reaper (inherited from ANTS-1351 § 2.4
  via the inline gate).
- **Sec M-D:** CSV grammar pinned (INV-14).
- **Sec M-E:** mtime recheck rate-limit (INV-15).
- **Sec M-F:** counter race closed via single batched
  `allocateIds(N)` call (INV-3).
- **Opt H1:** schema enum for dimensions dropped (INV-6); runtime
  validation only.
- **Opt H2:** patterns ship as project-internal JSON resource
  (INV-1); engine self-contained.
- **Opt H3:** InFlightRegistry class dropped; inline gate per
  ANTS-1351 § 2.4.
- **Opt H4 + user decision:** `brief` string field dropped (§ 2.3);
  caller composes prompt.
- **Opt M9 + user decision:** partition_token via qHash, not
  SHA-256 (INV-4).
- **Token H1/H2:** § 1 framing corrected (8 K not 14 K skill
  displacement, once per session).
- **Token H3:** net per-sweep savings honest table in § 1.
- **Token M1:** brief duplication killed by H4.
- **Token M2:** `pre_pass_findings_by_chunk` cached across
  partition pages (INV-10).
- **Final consistency:** chunks_count math example fixed
  (192/12 = 16); engine API structs fully specified (§ 2.6);
  `unknown_dimension` mapped to INV-6 in § 4; INV-5 + § 4
  `caller_cwd_required` scope clarified (write verb only); INV-13
  notes `pattern_id+file+line` is a same-uid read primitive (L-I).

v4 ready for sign-off. Implementation can proceed; remaining items
deferred per § 9.

**Post-implementation reconciliation (2026-07-25).** A cold-eyes lane run
against the *shipped* verbs (rather than the draft) found this spec had
drifted in four substantive ways. All are fixed above; recorded here
because loop 3's "v4 ready for sign-off" line above no longer describes
the document's state.

- **INV-5 said `Optional` on all four verbs; every verb ships
  `Required`.** All five `test_audit_*` registrations use
  `CallerCwdContract::Required`, and `callerCwdContractFor` lists them as
  such. The spec's claim that read verbs
  accept an absent `caller_cwd` was the reverse of the truth — a caller
  building to it would omit `caller_cwd` and be refused on every call.
  This is the one finding that would have actively misled an
  implementer. Corrected in INV-5, § 4, § 5 test 5, and § 8.
- **The `m_auditPool` worker model was never built.** § 2.7 and § 6 both
  described partition as running on a pool shared with `audit_run`.
  ANTS-3612 established the pool was always aspirational; `audit_run`
  now bounds itself with a flat engine-side cap instead, and partition
  runs *inline on the dispatcher thread*. § 2.7 already carried the
  correction; a leftover line in § 6 still asserted the pool twenty
  lines after § 6's own correction contradicted it. Both now say the
  same thing, and § 6 states the GUI-blocking consequence honestly.
- **`reports_dir_missing` is a retired code.** ANTS-1455 renamed it to
  `reports_dir_outside_root` and added `reports_dir_unreadable` /
  `reports_dir_empty`. § 4 listed only the retired name.
- **Renamed / unimplemented response keys.** `dimension_hints` became
  `pre_pass_dimensions` (ANTS-1487) in both the § 2.2 example and § 2.6's
  struct; `dimensions_skipped` and `skip_reason_per_dimension` are
  declared but never populated and are now marked as such rather than
  shown as shipped; `pre_pass_findings_by_chunk`'s omission rule gained a
  second trigger (envelope size, ANTS-2070) plus `pre_pass_chunk_ids[]`;
  `additional_frameworks[]` (ANTS-1623) was undocumented.

Also: the heading "Four-verb trio" was self-contradictory *and*
undercounted — a fifth verb, `test_audit_recheck`, shipped under
ANTS-1722 and is out of scope for this spec. Eleven refusal codes across
the audit and test-audit families were missing from
`docs/standards/mcp-error-codes.md` and were added there in the same
pass, so § 4's rows now all resolve to a taxonomy entry.

**Loop-3 reconciliation (2026-07-25).** A third cold pass found this
spec's § 2 had never been reconciled to shipped code — a systematic
staleness rather than scattered errors, and the reason two earlier
passes kept finding more:

- **INV-10 named a mechanism that does not exist** —
  `PaginationEngine::pageBullets`. `testauditengine.cpp` has zero
  references to it; the paging is a hand-rolled slice. This was the only
  claim in either doc pointing a maintainer at a non-existent call.
- **All four verbs' documented envelopes were short of shipped.** synth
  alone omitted four request fields and nine response fields; brief
  omitted `prior_false_positives`; fold_in omitted four request fields
  and two response fields; partition omitted three. § 2.6's struct
  listing — which carried an explicit "all struct fields explicit"
  completeness claim — was stale in six places. That claim is now
  replaced with a pointer to `src/testauditengine.h` as authoritative,
  because a listing that invites trust and then drifts is worse than one
  that defers.
- **§ 8 still recorded the inline in-flight gate as a live decision**
  one section after INV-12 was corrected to NOT IMPLEMENTED — the same
  restatement pattern, caught again.
- **§ 5's test 5 was marked corrected when the test was never built.**
  The 2026-07-25 pass fixed its *wording* from Optional to Required; the
  `mcp_caller_cwd_contracts` extension it describes does not exist. Now
  marked NOT BUILT — the contract is right in code and unasserted in
  tests.
- § 4 gained the two dispatcher-level refusals (`rate_limited`,
  `mcp_disabled`) its sibling spec already lists.

Filed from this pass: **ANTS-3625** — the `/test-audit` skill fast-path
was never applied, the exact twin of the ANTS-3613 gap fixed earlier the
same day.

**Loop-4 reconciliation (2026-07-25).** The fourth cold pass on this doc
returned *more* CRITICALs than the third, not fewer — because it was the
first to check § 4's code attributions and INV-14's validation claim
against source rather than for internal consistency. Fixed:

- **§ 4 attributed `no_project` and `no_roadmap` to `test_audit_fold_in`.**
  Neither is emitted by `foldIn`: an uncanonicalisable `caller_cwd`
  returns **`bad_path`**, and a missing/unwritable roadmap surfaces as
  `write_failed`. Both codes belong to `test_audit_recheck` — the verb
  this spec declares out of scope. A caller catching `no_project` from
  `fold_in` would wait for something that never arrives. The same
  mis-attribution had been mirrored into
  `docs/standards/mcp-error-codes.md` by an earlier pass in this loop and
  was corrected there too.
- **INV-14 asserted a security property the engine does not implement.**
  `PathValidation::validatePath` has exactly **one** call site
  (`testauditengine.cpp:1193`), for `reports_dir`. `scope:"path:<sub>"` is concatenated straight onto the
  project root with no anchor, so `path:../..` walks outside the project;
  `files:<csv>` refuses only on a backslash anywhere in the scope
  string; an empty CSV entry is silently skipped, not refused, and
  `../` passes.
  The invariant now describes the shipped behaviour and flags the gap;
  the code fix is **ANTS-3627**. Note § 5's test 14 — the test that would
  have caught this — was among those never built.
- **§ 5 test 5 said "unasserted in tests"; the truth was worse.**
  `Inv5OptionalContract` ships and asserts the *inverse* of the shipped
  contract, passing only because its comment-literal scrape window
  drifted off the block it checks as the contract table grew. It would
  pass either way. Loop 3 read a vacuous test as no test — they look the
  same until you ask why it is green. Tracked as **ANTS-3628**; tests 2,
  11 and 14 are genuinely absent, and shipped test names drift from the
  plan's.
- **§ 2.4's response fence closed in the wrong place** — an artifact of
  loop 3's own edit, which rendered the prose that follows it inside the
  JSON block.
- Smaller: INV-10 kept a `paginationengine.h` citation fragment after the
  mechanism claim was corrected; § 2.5's fold_in request example (the
  block an implementer copies) omitted four accepted keys; the
  `calibration_anchor` example carried a `noise_rate_pct` the engine
  ignores; `next_offset` is emitted unconditionally with a `-1` sentinel,
  not optionally; § 4's `rate_limited` row missed that `synthesis_prompt`
  is Cheap-tier; and § 1's byte figures were ~38 KiB measured against
  "~32 KiB" claimed.

§ 7's PROPOSED banner is now APPLIED — ANTS-3625 shipped the
`/test-audit` fast path earlier the same day.

**Loop-5 reconciliation (2026-07-25).** Five CRITICALs — the highest
count of any pass on this doc, and not because earlier fixes regressed.
Each loop has probed a different dimension; loop 5 was the first to check
INV-4 and INV-15's *freshness mechanisms* against source, and both
describe machinery that was never built:

- **INV-4's token recompute does not exist.** `computeToken` is called
  from `partition()` only. `brief` and `synth` do a bare lookup against
  an in-memory LRU (cap 16). So `partition_token` detects eviction or a
  process restart — **never a changed file set**. Edit a test file and
  the same token still resolves. The loop-3 security-M8 "recompute before
  any cache lookup" ordering describes a guard that isn't there; §4 and
  §5 test 4 restated it and are corrected too, as is the mirrored
  `stale_partition` row in the error-code taxonomy.
- **INV-15's mtime recheck does not exist either.** `mtimeWalkComputedAt`
  is written once and never read; the "recheck" block stats nothing and
  carries an in-code comment saying v1 skips it. Only the 5 s rate-limit
  timestamp shipped — throttling a check that never happens. The
  "deep-tree mtime gap" the original text conceded understated it: the
  gap is total.
- **The taxonomy's `no_project` / `no_roadmap` rows still named
  `test_audit_fold_in`** — which emits `bad_path` and `write_failed`
  respectively. The loop-4 log claimed this was already corrected here;
  it was not (see the process note below).
- **§7's PROPOSED banner** contradicted the skill file, the ROADMAP and
  this doc's own loop log. Also never landed in loop 4.
- **§4's `bad_path` row** listed three conditions that don't emit it and
  omitted the three that do.

Also: INV-7 promised a `DBGLOG` warning on pre-pass cap overflow — the
engine has no logging call at all, and `Inv7PrePassCap` is a source
scrape that asserts neither the behaviour nor the warning; `files:<csv>`
refuses only on a backslash (empty entries are silently skipped);
`reports_dir_empty` accepts `*.json` as well as `*.md`; and the "parity
with audit_run" citation quoted in three places belongs to `audit_poll`.

**Process note.** Three loop-4 edits to this file and two to the taxonomy
never landed: a multi-edit batch aborted on one failed match and
discarded its remaining writes, and the loop-4 log recorded them as done
without re-reading the files. Fixed here, and the log entry above is
written from verified file state rather than from intent.

**Loop-6 reconciliation (2026-07-25).** The first pass to walk INV-1…15
one at a time against source. It produced both the largest finding set
and — more usefully — the first **verified-correct list**, which pins
most of this document: INV-3, INV-4, INV-5, INV-7, INV-8, INV-10, INV-12,
INV-13 and INV-14 are all now confirmed accurate against the engine, as
are § 4's rate-limit tiers, the `reports_dir_*` trio, § 2.1/§ 2.2's key
lists, § 2.5, § 2.6's disclaimer, § 1's byte figures and § 7's APPLIED
banner. Those need no re-checking.

What was still wrong:

- **`quick` is a documented no-op (ANTS-3630).** Declared, forwarded from
  the dispatch lambda, never read by the engine — `quick:true` does the
  identical full walk and full pre-pass scan. This is the only finding
  with a user-visible wrong behaviour: `/test-audit --quick` advertises
  "seconds not minutes". Worse, ANTS-3625 had *just* applied § 7's flag
  mapping to the live skill, so a stale spec line became live
  instructions to rely on a dead flag. The skill now does the `--quick`
  skipping caller-side and says the flag is inert.
- **INV-1's alternation does not exist.** No longest-first ordering, no
  common-prefix factoring, no per-language combined regex — the engine
  compiles 17 separate patterns once per process and loops them per
  line. § 6's "single combined regex per file" and § 5 test 1's
  "AlternationOrder" half went with it.
- **§ 6 still claimed "subsequent partitions inside token TTL ride
  cache".** There is no TTL, and `partition` never reads the cache — it
  always re-walks. Third occurrence of the restate-after-correction
  pattern this log names twice; caught before it became a loop-7 finding.
- **Four response shapes in § 2.3/§ 2.4 were invented or wrong** —
  `framework_context` listed three fields the engine never sets;
  `top_dimensions` is an array of objects not strings; `file_index` is an
  array not a map; `severity_histogram` keys are lowercase. These are the
  blocks an implementer copies, so each would break at the first call.
- **`bad_actionable` is not what an empty `actionable[]` returns** —
  that is `missing_field`; `bad_actionable` fires on an entry missing its
  headline. The taxonomy's example named exactly the case that does not
  produce it.
- **§ 5's coverage claim was materially overstated.** Ten of the eleven
  shipped tests are source-scrapes, not the runtime assertions the items
  describe. § 5 now says so per-item and names the four sibling runtime
  suites it had never mentioned — including
  `test_audit_pagination_prepass/`, which exercises the very INV-10
  behaviour item 10 claims is unbuilt.
- Smaller: paths are absolute throughout (every example implied
  project-relative); `chunk_size` clamping is not echoed; `missing_field`
  is not emitted by `partition`; the `bad_path` backslash guard only
  applies to `files:`-prefixed scopes; `no_tests_found` has a third
  trigger; the pre-pass pattern count is 17 not 30;
  `pre_pass_chunk_ids[]` is unconditional; and partition's `byte_count`
  measures a two-key stub, so the example figure was off by two orders
  of magnitude.

**Loop-7 reconciliation (2026-07-25).** **Zero CRITICAL** — the first
pass on this doc to reach that — and all six remaining priority
invariants (INV-1, INV-2, INV-6, INV-9, INV-11, INV-15) verified correct
against source, alongside a re-verification of INV-5, § 4's rate tiers,
§ 2.4's shapes and § 7's banner. `docs/standards/mcp-error-codes.md` came
back **clean for this family** with no findings. Between loops 6 and 7,
every invariant in this spec has now been individually checked against
the engine.

Fixed this pass:

- **§ 6 and § 5 test 1 still sold INV-1's alternation design** — "longest-
  first + common-prefix-factored", "single combined regex per file",
  "alternation ordered longest-first". INV-1 had retracted all of it in
  loop 6; these two restatements survived.
- **§ 7's flag mapping still routed `--quick` to `{quick:true}`** while
  § 2.2 declared the flag inert *and* asserted "§ 7's flag mapping is
  annotated accordingly". It was not.
- **`additional_frameworks` was documented as a string array**; the
  engine emits `[{name, root_rel}]`, and the key is omitted when empty.
  A copy-paste block that breaks on the first polyglot repo.
- **INV-2's "≤ 8 KiB per chunk" is unenforced and exceedable ~8×** —
  nothing truncates, and `prior_false_positives` (up to 50 entries ×
  ~1.3 KB) is unbounded. Restated as a target with the unbounded term
  named.
- **The "paths are absolute" note over-generalised** — `file_index` keys
  are scraped from report *text* and merged by basename, so they are
  whatever the subagent wrote.
- **The ANTS-1447 deep-tree paragraph read as shipped behaviour** in
  present tense, twenty lines below INV-15's retraction of the entire
  recheck. Now marked superseded.
- Smaller: `bad_path` has a fourth condition (a present-but-malformed
  `reports_dir`); the request/response examples disagreed on `limit`;
  INV-6 named a variable that is an accessor; `prior_false_positives`
  entries carry a `timestamp`; and the "≤ 400 ms budget above" referred
  to a figure that appears nowhere.

**Process note — same failure, second time.** Three of loop 6's recorded
fixes had not landed: the § 6 alternation lines, § 5 test 1, and the § 7
annotation § 2.2 claimed existed. Loop 4 hit this too. The cause both
times was writing the log entry from the *finding list* rather than from
the file. This entry was written after grepping each of the twelve
changes back out of the file to confirm it is there. That is the only
version of "fixed" worth recording.

**Loop-8 reconciliation (2026-07-25) — CONVERGED.** **Zero CRITICAL,
zero HIGH**, and `docs/standards/mcp-error-codes.md` returned no findings
for this family at all. Every invariant in this spec has now been
individually verified against the engine across loops 6-8. The remaining
findings were polish, and are fixed here:

- `file_index` was typed `{}` in § 2.1's return column while § 2.4 said
  "an array, not a map" three sections later.
- INV-14 claimed `files:<csv>` "rejects empty entries and backslashes";
  the engine refuses only when the *scope string* holds a backslash and
  silently skips bad entries — § 4 already said so.
- INV-10's "other three verbs always fit under 20 KiB" contradicted
  INV-2, which had just been corrected to say `brief` is unbounded.
- § 6's fold-in budget quoted ROADMAP.md at 763 KiB; it is 2.44 MiB
  today, so the wall-time and RAM lines were understated ~3.3×. This
  project treats stated RAM budgets as contract, so a stale one is worth
  a finding.
- `test_audit_pattern_drift/` is in the `test_core` bundle, not
  `test_claude` — `ctest -R test_claude` misses the INV-1 drift guard.
- Smaller: `validatePath` has one call site not two; § 5's scrape list
  wrongly included test 6 (a genuine runtime call) and omitted test 5;
  the § 2.2 example omitted `pre_pass_chunk_ids` which the prose says is
  unconditional; § 2.7 rebutted prose that had already been deleted;
  "remains a draft" contradicted the status header; and the retained
  ANTS-1447 pointer should name ANTS-1450, which holds the actual open
  wiring (ANTS-1447 shipped doc-only).

**Convergence.** Loops 1-8 took this spec from a draft-era document whose
invariants had never been checked against the shipped engine to one where
each has been. The audit trail is worth stating plainly: six genuine code
gaps were found and roadmapped along the way (ANTS-3624, 3626, 3627,
3628, 3629, 3630), of which two are security-relevant and one —
`quick` — is user-facing. Three loops were partly spent re-fixing edits
that had not landed, a self-inflicted cost recorded in the loop-6 and
loop-7 process notes.
