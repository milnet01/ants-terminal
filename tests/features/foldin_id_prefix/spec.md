# foldin_id_prefix — ANTS-3473 conformance

The fold-in ID renderers (`test_audit_fold_in`, `cold_eyes_fold_in`,
`indie_review_fold_in`, and the `plan_template` ID) used to hardcode
`ANTS-<n>` when stamping a newly-allocated roadmap ID. A fold-in into a
project whose roadmap uses a different prefix — e.g. finbreak's
`FIBR-NNNN` — produced wrong-prefix bullets that collide with the real
Ants roadmap's own `ANTS-NN` IDs and break that project's
`.roadmap-counter` contract. `roadmap_log op:append_batch` already sniffs
the right prefix in the same project, so the fold-in verbs were the odd
ones out.

## Fix

`RoadmapFoldIn::sniffIdPrefix(projectPath, fallback="ANTS")` returns the
dominant `[PREFIX-NNNN]` bracketed prefix from the project's ROADMAP.md
(the same dominance-by-count sniff `corpusHighWater` already used —
extracted into a shared `sniffPrefixFromText` core, so both read the
roadmap once). Each fold-in caller sniffs its project's prefix and threads
it into the renderer:

- `templateColdEyesFoldInBlock` / `templateIndieReviewFoldInBlock` gain a
  trailing `idPrefix` param defaulting to `"ANTS"` — so existing 3-arg
  callers and their unit tests stay byte-identical.
- `test_audit_fold_in` (testauditengine.cpp) and `plan_template`
  (plantemplateengine.cpp) render `%1-%2` with the sniffed prefix.

## ANTS-3480 residual — zero-padding + uniform `allocated_ids`

ANTS-3473 got the *prefix* right but left two residuals (finbreak tester,
build c57e16b6):

1. **Un-padded suffix.** The fold-in renderers stamped `[FIBR-82]` while
   `roadmap_log op:append` (same `.roadmap-counter`) renders `[FIBR-0082]`,
   matching every existing bullet. Any tooling matching `/FIBR-\d{4}/` or
   lexicographically sorting missed/misordered the un-padded ids.
2. **`allocated_ids` type drift.** `test_audit_fold_in` returned
   `["FIBR-82"]` (string) while `cold_eyes_fold_in` / `indie_review_fold_in`
   returned `[82]` (bare int).

Fix: one shared renderer `RoadmapFoldIn::renderId(prefix, n)` =
`"%1-%2".arg(prefix).arg(n, 4, 10, '0')` — byte-identical to op:append's
render. All three block templates and all three verbs' `allocated_ids`
echo route through it, so the id a caller reads back equals the id written
into the block, uniformly as a padded string.

## Invariants covered

- **INV-1** — `sniffIdPrefix` returns the dominant bracketed prefix
  (`FIBR` over a stray `[UTF-8]`).
- **INV-2** — fallback (`"ANTS"` default, or a caller-supplied value) when
  ROADMAP.md is absent or carries no counter-style id.
- **INV-3** — `templateColdEyesFoldInBlock` stamps the passed prefix,
  zero-padded (`[FIBR-0077]`); the 3-arg default renders `[ANTS-0077]`.
- **INV-4** — `templateIndieReviewFoldInBlock`, same (`[FIBR-0088]`).
- **INV-5** (ANTS-3480) — `renderId` zero-pads the numeric suffix to a
  minimum of four digits (`FIBR-0082`, `ANTS-0001`); a suffix already ≥4
  digits is emitted verbatim (`ANTS-12345`, never truncated).
- **INV-6** (ANTS-3498) — `RoadmapFoldIn::isValidIdPrefix` accepts the
  canonical prefix grammar (ANTS-3492): 1-16 chars of `[A-Za-z0-9_-]`
  containing ≥1 letter. Digit-led is fine iff a letter is present
  (`3D_E` ✓); a letter-free (`2026`), empty, over-16, or whitespace-bearing
  prefix is rejected. Single-sources the shape shared with `roadmap_log`
  op:append's `kIdPrefixShape`.
- **INV-7** (ANTS-3498) — `TestAuditEngine::foldIn` honours an explicit
  `idPrefix` override (winning over the ROADMAP sniff), and refuses a
  letter-free override with `bad_args` *before* any `.roadmap-counter`
  touch. (The `cold_eyes_fold_in` / `indie_review_fold_in` verbs carry the
  same `id_prefix` request-param + validation in `remotecontrol.cpp`,
  validated before `allocateIds` for the same reason.)

## ANTS-3497 / ANTS-3498 residuals — closed

Two ANTS-3480 "out of scope" items were later implemented:

- **ANTS-3497** — `debt_sweep_defer` (and the GUI debt-sweep dialog) now
  thread the sniffed, padded prefix through `templateDebtSweepFoldInBlock`
  (which gained a trailing `idPrefix` param), and its `allocated_ids` echo
  routes through `renderId` — so it matches the other three fold-in verbs
  instead of hardcoding `[ANTS-<n>]` un-padded.
- **ANTS-3498** — the three fold-in verbs (`test_audit` / `cold_eyes` /
  `indie_review_fold_in`) gained an optional `id_prefix` request-param
  override (parity with `roadmap_log` op:append), validated against the
  shared `isValidIdPrefix`. Empty = sniff as before; non-empty wins.
  `debt_sweep_defer` was intentionally left sniff-only (outside the
  three-verb ANTS-3498 scope).

## Out of scope

- The `.roadmap-counter` numeric allocation is unchanged (`corpusHighWater`
  already floored correctly); only the rendered id *string* was wrong.
