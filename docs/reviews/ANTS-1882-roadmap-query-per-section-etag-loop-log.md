# ANTS-1882 — `roadmap_query` per-section ETag — review loop log

Record for [`../specs/ANTS-1882-roadmap-query-per-section-etag.md`](../specs/ANTS-1882-roadmap-query-per-section-etag.md) § 8. Cold-eyes loop log.

## Cold-eyes loop log

- **Loop 1 (2026-05-26).** Reviewer flagged H1 (duplicate_ids is
  a cross-section field — Vestige's per-section invariance claim
  doesn't hold without scoping the diagnostic), H2 (INV-2 grep
  anchor too vague — the section branch has multiple
  `return QJsonDocument(out)` exits), M1-M3 polish. Investigation
  reframed from "no code change needed" to "small targeted fix
  closes the gap". Fixes: added
  `rcFilterDuplicateIdsForSection` helper; section path emits
  scoped `duplicate_ids` only; INV-1..INV-4 rewritten with
  concrete test anchors; §2.3 explicitly names the other
  emission paths' unchanged behaviour.
