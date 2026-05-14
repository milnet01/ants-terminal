# ANTS-1287 — mcp_roadmap_section_slice feature test

## What it locks

The MCP-layer wiring for the `section` argument added to
`roadmap_query` per `docs/specs/ANTS-1287.md`:

1. **REG-1.** `"section"` arg present in the `roadmap_query` MCP
   registration block in `claudeintegration.cpp`.
2. **REG-2.** Schema declares `section` as a string property; the
   `required` array is not set (back-compat with INV-1).
3. **REG-3.** `cmdRoadmapQuery` in `remotecontrol.cpp` extracts
   `req.value("section")` and routes non-empty values through the
   `RoadmapIndex` slice path.
4. **REG-4.** `bad_section` error code present in `remotecontrol.cpp`
   for unknown slugs.
5. **REG-5.** Hygiene: the `bad_section` branch caps the verbatim
   echo at 64 bytes and replaces `< 0x20` bytes with `'?'`
   (parity with ANTS-1247 INV-11 / `bad_status`).
6. **REG-6.** Section cache members declared in `remotecontrol.h`:
   `m_roadmapIndex` (`QVector<RoadmapIndex::Section>`) and
   `m_roadmapSectionCache` (`QHash<QString, QJsonArray>`).
7. **REG-7.** Provider lambda in `mainwindow.cpp` forwards a
   non-empty `section` arg through `cmdRoadmapQuery`.

## Test shape

Source-grep, no GUI, no QTemporaryDir. Same pattern as
`mcp_token_usage_tool` / `mcp_roadmap_status_filter`.

Region of interest in `claudeintegration.cpp`: from the comment
`// ANTS-1287` (registration block extension) to the call to
`tools.append(roadmapTool)` immediately after.
