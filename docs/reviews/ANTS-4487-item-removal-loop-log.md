# ANTS-4487 — cold-eyes loop log

## Cold-eyes loop log

| Loop | Date | Model | Lanes | Q1 | Q2 | Q3 | Q4 | Verified | Fixed | Outcome |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2026-09-20 | Opus 5 | 3 | 0 | 4 | 3 | 2 | 9 | 9 | All author's own; the heaviest of the four. The decisive one: flipping to `dropped` — § 3's recommended route — writes a `history` row, and the draft made any history row a removal guard, so the primary route permanently foreclosed the exception route. Resolved by making `history` a cascade step rather than a guard, which also made INV-6 falsifiable. `dry_run` said 'without opening a write' while INV-7 required measuring inside the transaction; the transaction wins, per this project's thrice-learned lesson. Case-sensitivity contradicted `findItem()`'s `id_fold = lower(?)`. INV-7's byte-identity assertion is unsatisfiable on a WAL store. Four refusal codes were unnamed. |
