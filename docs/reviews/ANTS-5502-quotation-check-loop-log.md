# ANTS-5502 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-5502-quotation-check.md`](../specs/ANTS-5502-quotation-check.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-09-29 | 2 (neutral-lane, each held all four questions) | 0 | 1 | 1 | 2 | Verified 4, fixed 4, dismissed 3. Both lanes found the not_run/exit-code collapse (added `check_errors`) and the unpinned git cap (now `git cat-file blob <ref>:./<path>`, cap 16 MiB+1, `./` measured necessary from a subdirectory). Lane 2: INV-1 vectors could not catch step reordering (script run: `a ** b` vs `a b` and `[a]**(b) x` vs `a x` both MISS; vectors added). Lane 1: INV-5 passed a raw-string glob match (symlink case added). Dismissed: `compact` dropping an empty `findings` (fails closed at 5506 exit 2), `unreadable` fixture under root (a failing test the builder sees), `validatePath` escape routing (local). |
