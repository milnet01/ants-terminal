# Feedback ship dates — feature-conformance contract

This bundle held `feedback_log op:"compact_resolved"`
([`docs/specs/ANTS-3443.md`](../../../docs/specs/ANTS-3443.md)). ANTS-4513
retired that op and its cases
([`docs/specs/ANTS-4513-feedback-corpus-store.md`](../../../docs/specs/ANTS-4513-feedback-corpus-store.md)
§ 7). The ship-date cases stay, because `feedback_query` still reads ship
dates ([`docs/specs/ANTS-3504.md`](../../../docs/specs/ANTS-3504.md)).

## What this test asserts

- **Extractor** — `FeedbackFile::shipDateFromRoadmapBody` reads the date on
  the last line-leading `Resolved` line, with or without parentheses. It
  returns "" for a mid-line `Resolved`, `Resolved as of`, prose, or no
  `Resolved` line.
- **feedback_query** — `mapped_id_status` carries `shipped_date` on a ✅ id
  with a Resolved date, and omits it on a non-✅ id.
- **Wiring** — the remotecontrol sources call the shared extractor, and the
  `feedback_query` description documents `shipped_date`.
