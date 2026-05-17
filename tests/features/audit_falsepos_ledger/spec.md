# spec.md — `.ants_review_falsepos.jsonl` ledger (ANTS-1457)

Feature-conformance test for the false-positive ledger helper
introduced by ANTS-1457. Canonical spec at
[`docs/specs/ANTS-1457.md`](../../../docs/specs/ANTS-1457.md);
standard at
[`docs/standards/audit-false-positives.md`](../../../docs/standards/audit-false-positives.md).

## Coverage

| Group | Invariants pinned |
|-------|-------------------|
| G-2 / G-3 / G-4 | INV-1, INV-2, INV-3, INV-4 — load / missing / per-line skip / oversize line |
| G-5 / G-6       | INV-6, INV-6a, INV-7 — validation + surrogate-safe truncation |
| G-7             | INV-9 — bidirectional empty-match filter |
| G-8 / G-9       | INV-10, INV-11 — empty input + canonical header |
| G-10 / G-21     | INV-13 — fence escape on both claim and rationale |
| G-11            | INV-14 — 50-entry cap, oldest dropped |
| G-12            | INV-15 — 64 KiB block-size cap + sentinel |
| G-13            | INV-16, INV-17 — JSON array shape + cap |
| G-14 / G-15     | INV-18, INV-19 — IndieReview brief / dispatch include block |
| G-16            | INV-20 — Cold-eyes manifest brief includes block |
| G-17            | INV-21, INV-22 — TestAudit BriefResult populates `priorFalsePositives` + envelope key |
| G-18            | INV-5 — 1 MiB warning, full parse |
| G-19            | INV-8 — forward-compat unknown field |
| G-20            | INV-12 — per-entry sentinel markers |
| G-22            | INV-6a — validate before truncate |
| G-23            | INV-7 — combining-mark safety |
| G-24            | parser tolerance — BOM + CRLF |
| G-25            | INV-1a — non-regular file safety (`S_ISREG`) |
| G-26            | INV-6 — `review_kind` enum guard |
| G-27            | INV-6 — non-string-typed required field |

## Approach

Runtime tests against `QTemporaryDir` fixtures (no committed
ledger fixtures). Each test creates the project root with the
appropriate `.ants_review_falsepos.jsonl` content, calls into the
helper or engine, and asserts the expected behaviour. Engine-
wiring tests (G-14/15/16/17) additionally source-grep the
relevant cpp files to confirm the include + call-site survives
file-level changes.
