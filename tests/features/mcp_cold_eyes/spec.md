# mcp_cold_eyes — feature-conformance spec

**Owner:** ANTS-1319 (`docs/specs/ANTS-1319.md`)
**Sources under test:** `src/claudeintegration.cpp`,
`src/remotecontrol.{h,cpp}`, `src/mainwindow.cpp`.

Source-grep regression test (no runtime), mirrors
`tests/features/mcp_roadmap_section_slice/`. Verifies the four
`cold_eyes_*` MCP tools are wired: schema declarations, error-code
strings, args extraction, INV-11 echo hygiene, provider lambdas.

ANTS-5485 removed `cold_eyes_cross_doc_diff` and `cold_eyes_fold_in`.

## Cases (REG-1..REG-7)

| # | Case | Asserts |
|---|---|---|
| REG-1 | Tool names registered + `// ANTS-1319` anchor | `claudeintegration.cpp` contains the `cold_eyes_partition` and `cold_eyes_brief` blocks + anchor |
| REG-2 | Schema required-arrays match INV-10 | partition has none; brief→`["lane"]` |
| REG-3 | `cmdColdEyes*` extracts every arg via `req.value(…)` | lane / scope |
| REG-4 | `bad_scope` error code present (INV-8) | string `"bad_scope"` in remotecontrol.cpp |
| REG-5 | INV-11 echo hygiene on bad scope + lane | `verbatim.truncate(64)` + `< 0x20` substitute, OR shared helper |
| REG-6 | Cache members declared in header (INV-12) | `m_coldEyesCache`, `m_coldEyesCacheStampMs`, `kColdEyesCacheTtlMs` |
| REG-7 | Provider lambdas forward args (INV-9) | `cmdColdEyesPartition(args)` etc. wired in `mainwindow.cpp` |
