# ANTS-5502 — review loop log

The `review-contract` rows for [`docs/specs/ANTS-5502-quotation-check.md`](../specs/ANTS-5502-quotation-check.md).

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-09-29 | 2 (neutral-lane, each held all four questions) | 0 | 1 | 1 | 2 | Verified 4, fixed 4, dismissed 3. Both lanes found the not_run/exit-code collapse (added `check_errors`) and the unpinned git cap (now `git cat-file blob <ref>:./<path>`, cap 16 MiB+1, `./` measured necessary from a subdirectory). Lane 2: INV-1 vectors could not catch step reordering (script run: `a ** b` vs `a b` and `[a]**(b) x` vs `a x` both MISS; vectors added). Lane 1: INV-5 passed a raw-string glob match (symlink case added). Dismissed: `compact` dropping an empty `findings` (fails closed at 5506 exit 2), `unreadable` fixture under root (a failing test the builder sees), `validatePath` escape routing (local). |
| 2 | 2026-09-29 | 2 (neutral-lane, each held all four questions) | 0 | 1 | 3 | 2 | Verified 6, fixed 6, dismissed 3. Lane 1: tests drove a seam that did no reading (seam now owns validation, reads, git and the cache); per-item `outside_allowed` departs from `mcp-tools.md` step 4 unstated (now stated, INV-5 replaces the `bad_path` assert). Lane 2: STRIP/LINK paraphrased (patterns now verbatim, `x [a [b](c) y` vector, script HIT); steps 2/3 unpinned (`a \nb` vector, script HIT). From open questions: `ref` root check lexical, check order stated. Dismissed: ANTS-5506 exit 1 vs 3 precedence (that spec's gap, sent to its owner), `unreadable` under root, `validatePath` routing. Cap reached (spec cap 2): calm, 1 of 6 on text this run wrote (INV-1 vector list). Second share: none, the document is new in this run. No tail. |
