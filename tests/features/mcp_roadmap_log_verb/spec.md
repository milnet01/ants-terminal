# mcp_roadmap_log_verb — ANTS-1424

See `docs/specs/ANTS-1424.md`.

## Test scope

Source-scrape regression locks the `roadmap_log` MCP verb's
schema, classification, registration, and the cmdRoadmapLog
handler's invariants. Behavioural tests with a fixture
ROADMAP.md tempdir deferred to v2 of this ticket.

## Invariants checked

- **INV-1.** `roadmap_log` declared in the `tools/list` block
  in `claudeintegration.cpp` with status/kind enums + required
  fields.
- **INV-2.** `callerCwdContractFor` classifies `roadmap_log`
  as `Required`.
- **INV-3.** cmdRoadmapLog reads `.roadmap-counter`, computes
  `max(counter+1, id_hint or 0)`, writes back (anchor
  `ANTS-1424-INV-3`).
- **INV-4.** Insertion point uses RoadmapIndex (anchor
  `ANTS-1424-INV-4`).
- **INV-5.** Status → emoji map present (anchor
  `ANTS-1424-INV-5`).
- **INV-6.** Error paths return early before touching
  ROADMAP.md or `.roadmap-counter` (anchor `ANTS-1424-INV-6`).
- **INV-7.** Verb registered via registerToolProvider in
  mainwindow.cpp.
- **INV-8.** Success envelope carries id + file + line +
  bytes_written fields (anchor `ANTS-1424-INV-8`).
