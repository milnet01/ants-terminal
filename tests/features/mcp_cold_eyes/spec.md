# mcp_cold_eyes — feature-conformance spec

**Owner:** ANTS-1319 (`docs/specs/ANTS-1319.md`)
**Sources under test:** `src/claudeintegration.cpp`,
`src/remotecontrol.{h,cpp}`, `src/mainwindow.cpp`.

Source-grep regression test (no runtime), mirrors
`tests/features/mcp_roadmap_section_slice/`. Verifies the four
`cold_eyes_*` MCP tools are wired: schema declarations, error-code
strings, args extraction, INV-11 echo hygiene, provider lambdas.

## Cases (REG-1..REG-8)

| # | Case | Asserts |
|---|---|---|
| REG-1 | 4 tool names registered + `// ANTS-1319` anchor | `claudeintegration.cpp` contains all four `t["name"] = "cold_eyes_*"` blocks + anchor |
| REG-2 | Schema required-arrays match INV-10 | partition has none; brief→`["lane"]`; cross_doc_diff has none (XOR `reports`/`reports_dir` enforced at handler — ANTS-1509); fold_in→`["actionable"]` |
| REG-3 | `cmdColdEyes*` extracts every arg via `req.value(…)` | lane / scope / reports_dir / actionable / date_iso |
| REG-4 | `bad_scope` error code present (INV-8) | string `"bad_scope"` in remotecontrol.cpp |
| REG-5 | INV-11 echo hygiene on bad scope + lane | `verbatim.truncate(64)` + `< 0x20` substitute, OR shared helper |
| REG-6 | Cache members declared in header (INV-12) | `m_coldEyesCache`, `m_coldEyesCacheStampMs`, `kColdEyesCacheTtlMs` |
| REG-7 | Provider lambdas forward args (INV-9) | `cmdColdEyesPartition(args)` etc. wired in `mainwindow.cpp` |
| REG-8 | `RoadmapFoldIn::allocateIds` + `insertBlock` called from fold-in cmd | INV-6 |
