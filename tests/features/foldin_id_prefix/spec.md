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

## Invariants covered

- **INV-1** — `sniffIdPrefix` returns the dominant bracketed prefix
  (`FIBR` over a stray `[UTF-8]`).
- **INV-2** — fallback (`"ANTS"` default, or a caller-supplied value) when
  ROADMAP.md is absent or carries no counter-style id.
- **INV-3** — `templateColdEyesFoldInBlock` stamps the passed prefix;
  the 3-arg default still renders `[ANTS-<n>]`.
- **INV-4** — `templateIndieReviewFoldInBlock`, same.

## Out of scope

- An explicit `id_prefix` request-param override (the finding's "and/or"
  half): sniffing already makes the correct prefix the default, which
  fixes the reported bug. An override for the rare mixed-prefix roadmap is
  a small follow-up if it recurs.
- The `.roadmap-counter` numeric allocation is unchanged (`corpusHighWater`
  already floored correctly); only the rendered prefix string was wrong.
