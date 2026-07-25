# spec.md — ANTS-1455 glob walk + synth mode/pagination

Feature-conformance test for the three ANTS-1455 sub-fixes:

| Group | Pin |
|-------|-----|
| G-1 / G-15 | INV-1 / INV-3 / INV-3a — partition default walk honours `test_globs`, excludes app source |
| G-3 | INV-5 — `scope="path:"` / `scope="files:"` branches unaffected |
| G-4 / G-5 / G-6 / G-17 | INV-6 / INV-7 / INV-8 / INV-8a — `reports_dir` validation, opt-in flag, empty-dir refusal |
| G-7 / G-8 / G-9 / G-14 | INV-10 / INV-11 / INV-13 / INV-14 — `mode` arg + pagination shape |
| G-13 | INV-15 — `mcp-error-codes.md` carries the three new rows |
| G-18 | ANTS-3627 — a caller-supplied `path:` / `files:` scope is root-anchored |

**G-18 (ANTS-3627).** `partition()` concatenated the scope onto the
project root with no anchor check, so `scope:"path:../.."` walked a tree
outside the project and returned its file paths in the envelope. The
three cases pin: (a) `path:` escapes refuse `bad_path`; (b) a `files:`
list refuses as a whole when *any* entry escapes — it does not silently
drop the bad entry; (c) an in-tree `..` that resolves back under the root
(`path:tests/../tests`) is still accepted, so the check is an anchor and
not a substring ban. The fixture places a real sibling directory outside
the root, so an escape has something to find and a passing assertion
can't be an empty-walk artifact. Anchoring is lexical
(`QDir::cleanPath`); symlinks inside the root are not resolved.

Runtime fixtures via `QTemporaryDir`; no committed corpora.
See canonical spec at [`docs/specs/ANTS-1455.md`](../../../docs/specs/ANTS-1455.md).
