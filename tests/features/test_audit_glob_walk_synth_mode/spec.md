# spec.md — ANTS-1455 glob walk + synth mode/pagination

Feature-conformance test for the three ANTS-1455 sub-fixes:

| Group | Pin |
|-------|-----|
| G-1 / G-15 | INV-1 / INV-3 / INV-3a — partition default walk honours `test_globs`, excludes app source |
| G-3 | INV-5 — `scope="path:"` / `scope="files:"` branches unaffected |
| G-4 / G-5 / G-6 / G-17 | INV-6 / INV-7 / INV-8 / INV-8a — `reports_dir` validation, opt-in flag, empty-dir refusal |
| G-7 / G-8 / G-9 / G-14 | INV-10 / INV-11 / INV-13 / INV-14 — `mode` arg + pagination shape |
| G-13 | INV-15 — `mcp-error-codes.md` carries the three new rows |

Runtime fixtures via `QTemporaryDir`; no committed corpora.
See canonical spec at [`docs/specs/ANTS-1455.md`](../../../docs/specs/ANTS-1455.md).
