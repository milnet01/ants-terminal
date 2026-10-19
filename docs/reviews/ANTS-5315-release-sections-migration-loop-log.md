# ANTS-5315 release sections migration — review loop log

Record for [`../specs/ANTS-5315-release-sections-migration.md`](../specs/ANTS-5315-release-sections-migration.md) § Cold-eyes loop log.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|------|------|-------|----|----|----|----|---------|
| 1 | 2026-10-19 | 2 (every lane held every question) | 0 | 4 | 4 | 1 | 9 findings, 8 verified / 1 dismissed; 8 fixed. Q2: not-store-backed refusal code contradicted `mcp-error-codes.md`; "three" `sections_*` arrays against four listed (§ 2.6, INV-7); INV-9's range sentence against § 2.3's unreleased rule; INV-2 against § 2.5's re-plan. Q3: tag rule's "earliest commit" unpinned (now the lowest tag containing any subject commit, as the measurement did); § 2.3a placement read titles before the retitles (INV-10 fixture now has an out-of-order wave); the `placed_by` marker lost silently over the history cap (apply now refuses `store_failed`). Q4: INV-12's test could not see repeated stamps. Dismissed: an unpinned whole-call wall budget, a local choice nothing else binds to. Six open questions resolved clean. Packet ~36k tokens. Loop 2 dispatched. |
