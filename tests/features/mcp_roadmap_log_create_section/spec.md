# Feature: `roadmap_log op:create_section`

Part of **ANTS-1878**. Full design + invariants live in
[`docs/specs/ANTS-1878.md`](../../../docs/specs/ANTS-1878.md).

## Scope

- `RemoteControl::cmdRoadmapLogCreateSection` — handler for
  `op:"create_section"`.
- `RemoteControl::cmdRoadmapLogCreateSectionForTest` — test seam
  (m_main-independent, mirrors `cmdRoadmapLogFlipBatchForTest`).
- `RemoteControl::cmdRoadmapLog` dispatch ladder routing
  `op:"create_section"` to the new handler.
- `bad_op_combo` error string updated to include `create_section`.

## Invariants tested

- **INV-1** dispatch — source-grep `op == "create_section"` in
  `cmdRoadmapLog`.
- **INV-2** required fields → `missing_field`.
- **INV-3** `level` ∈ {2,3} → otherwise `bad_level`.
- **INV-4** unknown `after_section` → `bad_section`; case-only mismatch
  → `bad_case` + `canonical_slug`.
- **INV-5** slug computed via `RoadmapIndex::slugifyHeading` —
  source-grep + a behavioural assertion (title with mixed case +
  punctuation → expected slug).
- **INV-6** computed slug already present → `slug_collision`.
- **INV-7** `line == lineEnd + 1` (1-based heading line).
- **INV-8** source-grep: handler uses `QSaveFile::commit()` as the
  only write path.
- **INV-9** `unrecognised_format` gate (parseBullets empty AND
  bytes > `kRoadmapMinParseableSize`).
- **INV-10** `bad_intro` regex `^#{1,6}\s` — positive (`## stray`) +
  negative (`#1234 ref`) cases.

## Method

`QTemporaryDir` holds a synthetic `ROADMAP.md` per test. The test
calls `cmdRoadmapLogCreateSectionForTest(req)` directly, asserts the
envelope + the resulting file content. Source-grep tests check
dispatch wiring against the remotecontrol TUs.
