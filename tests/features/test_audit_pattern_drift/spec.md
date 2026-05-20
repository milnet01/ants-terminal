# TestAuditEngine — pattern/dimension drift guard (ANTS-1450)

Locks the in-tree source-of-truth resource
`docs/standards/test-audit-grep-patterns.json` to the compiled
`TestAuditEngine` tables, so the documented pattern set and the engine
can never silently diverge.

The JSON is the human-editable record the skill markdown points at; the
engine keeps its patterns compiled-in (it ships in a binary). This guard
is the seam that keeps the two honest.

## Invariants under test

- **INV-D1.** The JSON `dimensions` array equals `kDimensions()`
  exactly — same entries, same order.
- **INV-D2.** The JSON `patterns` array has the same length as
  `prePassPatterns()`, and for each index the `id`, `dimension`, and
  `regex` match exactly (regex compared after JSON unescaping).
- **INV-D3.** Every pattern's `dimension` is a member of the
  `dimensions` list (internal consistency of the resource).
- **INV-D4.** Every pattern `id` is unique.
