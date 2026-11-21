# ANTS-3492 — Digit-led (letter-containing) roadmap ID prefixes — review loop log

Record for [`../specs/ANTS-3492-digit-led-letter-containing-roadmap.md`](../specs/ANTS-3492-digit-led-letter-containing-roadmap.md) § Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-07-10)** — 2 cold reviewers (full-dimension + doc-vs-code
  accuracy). Tally: CRITICAL 2 · HIGH 1 · MEDIUM 2 · LOW 5 · INFO 4
  (verified 10 / unverified 0). Both reviewers independently found the same
  top defect. Fixed all verified findings:
  - C1/H1 — `rcIsNonconformingIdToken("2026-07")` test expectation was
    `false`; code yields `true` (`kIdIsh` is digit-permissive). Corrected
    the expectation + documented the `kCanonical`/`kIdIsh` asymmetry (only
    `kCanonical` widens) so an implementer can't collapse the ANTS-3387
    guard.
  - C2 — added `roadmap-format.md § 3.5.1` (the contract) as Surface row 0.
  - H1 — Problem "every ID recogniser" → "every *canonical* recogniser" +
    called out `kIdIsh`.
  - M1 — wrote out the prefix-only regex + `{0,15}`-cap-preserved note.
  - M2 — Surface row 2 symbol corrected to `sniffPrefixFromText`.
  - L1 — per-site digit-suffix bound (`{1,8}`) preserved note.
  - L2 — added empty-prefix / trailing-dash / underscore-lead boundary
    reject tests.
  - L3 — INV-2 scoped to canonical recognisers + reconciled with the flip
    guard's `bad_id_format`.
  - Row-10 label corrected to `rxLeadId` (summary-column strip); bold/bare
    fallback stays-letter-led note added to sites-LEFT.
- **Loop 2 (2026-07-10)** — 1 cold reviewer (full-dimension + doc-vs-code +
  completeness). Tally: CRITICAL 0 · HIGH 0 · MEDIUM 1 · LOW 3 · INFO 2
  (verified 4 / unverified 0). The loop-1 critical defect did NOT resurface
  (fix held). Reviewer independently re-verified the grammar correct, the
  `kCanonical`/`kIdIsh` asymmetry correct, and the WIDENED site list
  **complete** for the dashed-counter path. Remaining findings were all
  doc-completeness polish (no structural/mechanical/architectural defect) →
  **polish convergence**. Fixed: M1 — row 9 now names both `idTokenPattern`
  consumers (`parseBullets` + `parseShippedDates`) + the fragment splice
  recipe; L1 — stable-prefix sites-LEFT list completed
  (claudeintegration.cpp:8410, remotecontrol.cpp:5950); L2 —
  `rlMaxExistingIdForPrefix` added to the `{1,8}` digit-bound list; L3 —
  row-8 `:8620` `kIdPrefixShape` check confirmed. Converged.
- **impl (2026-11-13)** — ANTS-6236 amendment; no reviewer dispatched. The sites-LEFT entry for `prefix_hint` now says ANTS-6236 widened it to `kIdPrefixShape`. No invariant or test changed.
