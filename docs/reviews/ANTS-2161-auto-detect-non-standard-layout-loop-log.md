# ANTS-2161 — Auto-detect non-standard layout + `project_settings` verb — review loop log

Record for [`../specs/ANTS-2161-auto-detect-non-standard-layout.md`](../specs/ANTS-2161-auto-detect-non-standard-layout.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-06-24).** Two independent cold reviewers (spec-vs-code;
  spec-vs-standards). Verified + folded: Status token `draft` →
  `spec draft` (specs.md § 3.2/§ 5.6); the write-arg wire shape was
  ambiguous (flat keys vs. a nested `settings:{}` object) — pinned to
  FLAT top-level keys, `settings` wrapper removed from § 2.3 + INV-6;
  invented refusal codes reconciled with the taxonomy — `malformed_existing`
  → reuse `unrecognised_format`, `bad_args`-for-absence → `missing_field`,
  `already_exists` → NEW `settings_exists` (sibling of `plan_exists`),
  with `mcp-error-codes.md` added to § 7 and the per-op refusal sets
  enumerated; § 2.4 names the atomic-write helper (`secureio.h`'s
  `fsyncParentDir`) and notes write-time validation complements (not
  replaces) ANTS-2160's read-time consumer check; § 2.2 ETag wording
  softened ("stable while the tree is unchanged" — counts move with the
  source set); `session_orient` block citation refined to `:11668`.
  Dismissed (unverified): a "section-order drift from ANTS-2160" finding —
  the order is byte-for-byte the sibling's (Problem → Surface → Invariants
  → Tests → RAM → Out-of-scope → Cross-doc → Security).
- **Loop 2 (2026-06-24).** Two independent cold reviewers. Both
  independently caught the one real miss: INV-5 still named the
  pre-loop-1 code `already_exists` → corrected to `settings_exists`.
  Other verified folds: the "no recognised key" guard on `set` moved
  `missing_field` → `bad_args` (the taxonomy's combination-guard rule —
  no single field is required, so `missing_field`'s "wholly-absent
  required field" precondition doesn't hold); the reused
  `unrecognised_format` envelope now states it carries
  `expected_format:["json-object"]` + `hint` per the ANTS-1463 contract;
  `op:"init"` now states explicit keys SUPPRESS the detector (no merge)
  and that the chosen key-set (explicit or detected) always passes
  through `applyWrite` validation; a `null`-only `set` is pinned as a
  valid no-op write (not `bad_args`); `settings_exists` envelope carries
  `path`; the `admittedSuffix` → `isIndexableSuffix` rename + its
  `:113` call-site spelled out; wire-`snake_case` ↔ struct-`camelCase`
  mapping noted; INV-13 test surface given concrete grep literals; the
  § 2.4 strict-validation note refined to distinguish the UX upgrade
  (type-mismatch) from the escape/non-existence bound; a
  `**Composes with:** ANTS-2160` relationship line + a RAM
  walk-cost-is-gated clause added. Dismissed (unverified, sibling-
  consistent): the recurring Tests-before-RAM "skeleton order" nit (the
  order matches the accepted ANTS-2160).
- **Loop 3 (2026-06-24).** Zero CRITICAL/HIGH. Verified + folded: the
  `admittedSuffix` → `isIndexableSuffix` rename was reframed from
  present-tense fact to a planned rename, and its two stale comment
  references (`fileoutline.cpp:165`, `symbolquery.cpp:54`) named for the
  same-commit sweep; INV-3's fixture pinned to "no other source files"
  so `totalSourceCount == 2` is unambiguous; a § 6 line surfaced the
  `kSuggestLowWaterMark` nudge gate as a documented limitation; the
  refusal-table `caller_cwd_required` note made an explicit table caption;
  an invented "ConsumerWiring test" cross-reference dropped for the real
  source-grep surface; INV-9's escape/absent vs type-mismatch arms
  labelled; § 7's `settings_exists` registration spelled out with the
  taxonomy's meaning+example requirement.
- **Loop 4 (2026-06-24).** Zero CRITICAL/HIGH. Verified + folded: INV-10
  re-anchored to the verb's load/parse step (the malformed-file refusal
  precedes `applyWrite`, which only sees already-parsed JSON); the `init`
  refusal row gained `bad_args` (malformed explicit keys) + the narrative
  pinned the `reason`-field provenance in the explicit-keys path; the
  "counts cannot drift" claim softened to "per-file admission cannot
  diverge" (detect walks repo-wide, codebase_index walks src/+tests/ —
  shared admit rule, not traversal); the comment-reference quote
  misattribution corrected; INV-9's no-clobber arm clarified as a
  verb-layer/by-construction guarantee; minor citation polish
  (`cmdApplyEdits` write `~:8900`, `defaultSourceCount` never consults
  `source_roots`). Status line updated to carry loop progress.
- **Loop 5 (2026-06-24).** One verified HIGH: INV-13 / § 4 cited
  `remotecontrol.cpp` for the `CallerCwdContract::Required` registration,
  but that table is `src/claudeintegration.cpp::callerCwdContractFor()`
  (verified directly — `codebase_index`/`apply_edits`/`spec_log` sit in
  that `if (toolName == …) return C::Required;` chain at `:10003`+);
  `remotecontrol.cpp` holds only the handler. Both citations corrected.
  Folded MEDIUMs: an absent/unrecognised `op` → `bad_args` pinned (op-enum
  guard); the reused `unrecognised_format` envelope trimmed to exactly
  `expected_format[]`+`hint` (no silent `path` extension); `init`'s
  `written:false` no-op surfaced in the refusal table. LOWs: `bad_path`
  noted to key on `isInsideProject` (not the taxonomy's `validatePath`);
  the "type check (`candidates()`)" attribution tightened to the
  `QDir(...).exists()` dir-gate; Layman line de-jargoned; redundant
  `defaultSourceCount` parenthetical trimmed.
- **Loop 6 (2026-06-24).** Zero CRITICAL/HIGH; the loop-5 INV-13 fix
  verified correct against source. Folded two testability MEDIUMs: INV-8
  extended to lock the `null`-only `set` → `written:true` (not `bad_args`)
  boundary; new INV-14 added to pin the `kDominanceRatio` gate (a miss
  with no subdir set reaching the bar → no suggestion, contrast INV-3's
  100 % case). LOW: § 2.1 reworded — the export is a move out of the
  anonymous namespace + rename + header declaration, not a bare rename.
  Accepted at loop 6 (the residual items were testability strengthening,
  not factual or contract errors).

### ANTS-3369 amendment cold-eyes loop (2026-06-30)

A fresh loop over the 2026-06-30 amendment (suggest-all-subdirs on a miss,
always-non-empty `reason`, `wouldUseRoots` + `excluded` echo). Two
independent cold reviewers per loop; the doc-vs-code gaps the amendment
itself introduces (struct fields, retired `kDominanceRatio`, always-reason)
are tracked as the *implementation* TODO, not doc findings.

- **Loop 1 (2026-06-30).** 0 CRITICAL. Verified + folded: the §2.4
  `applyWrite` signature was missing `errCode` (5 vs 6 params — a
  pre-amendment doc-vs-code bug both reviewers caught); the `reason`
  contract pinned to non-emptiness only (wording advisory, so INV-3/14
  count-and-remainder phrasings aren't substring-asserted); INV-2 made the
  single home for the `present:true → wouldUseRoots` echo (INV-16 keeps
  only `excluded`, no duplicate fixture); `excluded` reworded from "by
  convention hold source" to "present on disk + skipped"; the count-desc
  sort's `name asc` tie-break restored to the doc-comment; §2.2 example
  reason format-matched; §4 gained INV-2.
- **Loop 2 (2026-06-30).** 0 CRITICAL. Verified + folded a sweep of stale
  `remotecontrol.cpp` line citations drifted ~700 lines: `cmdSessionOrient`
  `:11566`→`:12246`, `feedback_pending` `:11690`→`:12403`, INV-13's
  `callerCwdContractFor` row `:9232`→`:10003`, §2.4's `cmdApplyEdits` write
  `~:8900`→`~:9295`; the §2.2 example gained the `next_step` key the live
  nudge emits; detect's present-path behaviour on an unparseable settings
  file specified (`wouldUseRoots` nullopt, `reason` still non-empty, no
  refusal path); INV-1 strengthened to assert `wouldUseRoots` carries both
  `src` and `tests`.
- **Loop 3 (2026-06-30).** 0 CRITICAL. Verified + folded the stale §2.1
  `isIndexableSuffix`-export paragraph (the rename shipped 2026-06-24 —
  reframed "will be exported" → present-tense fact, call-site `:113`→`:115`,
  comment ref `fileoutline.cpp:165`→`:304`); §4 reworded to future tense
  (the test changes land with the implementation, not yet written); INV-11
  + §2.1 noise-dir lists reconciled to the single `isNoiseDir` source of
  truth (so INV-16's `excluded` contract is well-defined).
- **Loop 4 (2026-06-30).** 0 CRITICAL, 0 HIGH. Verified + folded: §2.1's
  `INV-11/13` cross-ref corrected to `INV-11` (the file-ceiling cap is an
  INV-11 property); the §2.2 example `reason` flagged illustrative-only;
  `excluded`'s scope collapsed to one canonical definition in INV-16
  (`isNoiseDir` matches minus dot-dirs — `build*` in, `.git`/`.ants` out);
  §2.3 names the `would_use_roots`/`excluded` wire-mapping site
  (`remotecontrol.cpp` ~`:9582`); this amendment loop log appended (the
  process gap a reviewer flagged — written as the loops run, not
  back-filled).
- **Loop 5 (2026-06-30).** 0 CRITICAL, 0 HIGH (the `--max-loops` cap).
  Verified + folded only micro-clarifications: `name asc` tie-break added
  to INV-14's sort phrasing (matching § 2.1); INV-3 vs INV-14 distinguished
  as distinct corners (single-subdir-no-remainder vs subdir-plus-root-
  remainder); the no-override `wouldUseRoots` membership rule pinned (the
  subset of {`src`,`tests`} that admitted ≥1 file); INV-16's `excluded`
  test broadened to assert a `*-deps` entry as well as `node_modules`; the
  § 2.1 struct-comment `excluded` gloss aligned to INV-16's "present on
  disk + skipped" wording. Both reviewers independently verified every
  line citation resolves and INV-14/15/16 are testable; no CRITICAL/HIGH
  across loops 4–5, findings shrank to phrasing-only. The amendment's
  doc-vs-code gaps (struct fields, retired `kDominanceRatio`, always-reason,
  the old tests) are the implementation TODO, tracked as Open Questions —
  resolved by the implementing commit, not the spec.
- **Loop 6 (2026-06-30) — confirmation pass.** A single cold reviewer
  returned ZERO CRITICAL/HIGH/MEDIUM/LOW: *"the spec is implementable as
  written … no factual, citation, consistency, or contract errors."* Every
  line citation re-verified; INV-1/2/3/11/14/15/16 confirmed mutually
  consistent + testable; the no-override `wouldUseRoots`-never-nullopt
  reasoning confirmed airtight. Folded the one INFO: INV-2's test now also
  asserts `!reason.isEmpty()` (the present branch of INV-15, previously the
  only enumerated branch without an explicit assertion). **Accepted at
  loop 6** — clean pass; the 5 Open Questions are the implementation TODO,
  not spec defects.

### ANTS-3390 amendment cold-eyes loop (2026-07-10)

Amendment: on a miss with source loose at the repo root, `detect()`
suggests `["."]` (the whole-root noise-pruned walk) rather than dropping
the depth-0 files as an un-suggestable remainder — closing the
RetroArch-class `codebase_index` coverage gap (§ status note, § 2.1,
INV-12/14/15/17). Runs cold until a pass returns zero verified findings.

- **Loop 1 (2026-07-10).** Two independent cold reviewers (spec-vs-code;
  invariant mutual-consistency + cross-doc). Both independently raised the
  same top finding: §6 "Out of scope" still carried a **Repo-root (`"."`)
  source layout — `detect()` suggests subdirs only** bullet (CRITICAL /
  HIGH) — the exact old behavior the amendment reverses, left stale when
  the Status note / §2.1 / invariants were updated. Verified + folded:
  deleted the §6 bullet; scoped INV-12 as INV-17's no-subdir degenerate so
  the INV-12/INV-17 conditions read as nested, not falsely disjoint (M);
  added an ANTS-3390 test-delta paragraph to §4 mirroring the ANTS-3369
  one (M); added a §2.1 rationale for whole-root `["."]` REPLACE over the
  roadmap's additive-non-recursive alternative (L); corrected the INV-13
  `callerCwdContractFor` citation `:10003`→`:10595` and the §2.1
  `walkSubtree` suffix-gate `:115`→`:116` (L). Fixture arithmetic
  (INV-14 `["app","engine"]`; INV-17 `["."]`) and the miss-space partition
  verified sound by both.
- **Loop 2 (2026-07-10).** Two independent cold reviewers, briefed cold
  (no prior-loop findings). **0 CRITICAL, 0 HIGH.** Both confirmed the §6
  conflict is gone, no stale OLD-behavior text survives in the doc body,
  and the INV-12/14/17 partition + fixture arithmetic + `validDirUnder`
  `"."`-acceptance are sound against code. Verified + folded the residual
  polish: reworded INV-14/INV-17's "takes precedence" → an explicit
  `rootLevel`-partition framing (the conditions are mutually exclusive, so
  "precedence" implied a phantom tie-break); reconciled §2.1's inline
  reason-branch list (was 5) with INV-15's six. **Polish-converged at loop
  2** — the only remaining verified findings are pre-existing citation
  drift OUTSIDE the ANTS-3390 blast radius (§2.2/§2.3 `remotecontrol.cpp`
  line numbers stale from ANTS-3468's session_orient move;
  `fileoutline.cpp:304`→`:356`), filed as a separate roadmap item rather
  than folded here. Loop 2's one code open-question — does a `["."]` walk
  key a bare `retroarch.c` or a `./retroarch.c`? — was **verified
  empirically** (a Qt `QDirIterator` probe): the walk base `<root>/.`
  yields `./`-prefixed `rel` values, which `findFile`'s exact-match
  lookup would reject. That would have silently defeated the amendment, so
  it is now a first-class contract point (**INV-18**): `walkSubtree` strips
  the leading `./`, also repairing shipped ANTS-3393's `["."]` keys. Caught
  before implementation — exactly the spec-before-code win the loop exists
  for.

### ANTS-3588 amendment cold-eyes loop (2026-07-24)

Amendment: when `detect()` already recommends a `source_roots` override, it
also proposes the conventional auxiliary layout keys that exist on disk
(`test_roots`/`docs_dir`/`specs_dir`/`roadmap`/`changelog`) so one `op:init`
writes the whole block (§ status note, § 2.1, § 2.3, INV-19). Additive — no
change to the no-override/standard-layout path. Runs cold until a pass
returns zero verified findings.

- **Loop 1 (2026-07-24).** Two independent cold reviewers (spec-vs-code
  accuracy + implementability; internal-consistency + conflicts). Findings,
  all verified + folded: **(HIGH)** the §6 "Out of scope" bullet still declared
  auto-detection of `docs_dir`/`roadmap`/`changelog`/`specs_dir` out of scope —
  the exact behaviour the amendment adds (the same stale-§6 defect class the
  ANTS-3390 loop caught); rewritten to "aux keys ride only on a `source_roots`
  suggestion; a STANDARD layout is still out of scope." **(MEDIUM)** INV-19's
  "writes them unvalidated" conflicted with §2.3's "detector-derived keys run
  through `applyWrite`" — reconciled by contracting `op:init` to route the whole
  detector-derived block (`source_roots` + aux) through `applyWrite` (the TOCTOU-safe
  direction; makes §2.3's pre-existing claim true), INV-19 reworded to "pass as a
  formality; a vanished path aborts with `bad_path`." **(MEDIUM)** the INV-19
  fixture (1 `engine/` file) would not force a miss — `1 ≥ kMissRatio·2` is a
  HIT; pinned to `engine/` ≥2 `.c` files, and noted `.md` is not an indexable
  suffix so `docs/`/root `.md` add no source count and `docs/` is never
  double-proposed. **(LOW)** `testRoots` order fixed to `tests`-then-`test`;
  §2.3 serialisation-site pointer extended to the five aux emissions;
  "either miss branch — INV-12/14/17" reframed as the `["."]`/subdir partition.
- **Loop 2 (2026-07-24) — confirmation pass.** One strong cold reviewer, both
  lenses, briefed cold (no prior-loop findings). **0 CRITICAL, 0 HIGH.** Verified
  `.md`∉`isIndexableSuffix` and `applyWrite`'s recognised key-set against code;
  confirmed the seven-mention key-set is uniform, the "ride only on a suggestion"
  claim consistent, and no INV-1/INV-6/§6 conflict. Folded the two polish
  findings: **(MEDIUM/clarity)** the §2.1 struct comment "nullopt/empty" could
  invite a present-but-empty value that `applyWrite` rejects (`bad_args`/`bad_path`)
  — reworded to nullopt-only, never present-but-empty; **(LOW)** the nullopt-branch
  enumeration rested on 3-of-5 branches — re-based on the governing "whenever
  `sourceRoots` is set" rule (§2.1 + INV-19). One code-side open question — the
  §2.3 `remotecontrol.cpp ~:11985` anchor re-drifted ~100 lines after the last
  commits (ANTS-3586…3595), the same `~`-approximate citation drift ANTS-3470
  re-synced 2026-07-21 — is OUTSIDE the ANTS-3588 blast radius and filed as its
  own roadmap follow-up, not folded here. **Polish-converged at loop 2** — the
  only remaining verified findings were wording precision; no
  structural/mechanical/architectural finding survived. The doc-vs-code gaps (the
  5 new struct fields, detect aux-population, verb serialisation, and the
  `op:init`→`applyWrite` routing) are the implementation TODO, resolved by the
  implementing commit.
- **impl ANTS-6668 (2026-11-28)** — Not a review loop; no reviewer was dispatched. INV-11 and the detect() comment now say the root's own files are counted even after the subdirectory walk spends kDetectFileCeiling, matching the fix that ProjectSettingsVerb.Ants6668RootSourceCountsPastTheBudget locks (seen red on the old code).
