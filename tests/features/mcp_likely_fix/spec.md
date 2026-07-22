# mcp_likely_fix — `likely_fix` add_include hint for undeclared-symbol diagnostics

**Status:** shipped
**Kind:** feature
**Source:** cc-feedback-2026-06-30 (Vestige Sug-C) — ANTS-3374

## Problem

`recent_errors` and `build_status` surface compiler diagnostics but stop
there. The single most common C++ build error — an undeclared symbol from a
missing `#include` (`'DeviceHotSwapMode' has not been declared`) — leaves the
Claude session to run a second `find_definition(X)` verb by hand to learn which
header to add. That two-verb diagnose→fix loop is stitchable: on such a
diagnostic, resolve the symbol's declaring header and attach it inline.

## Surface

New pure helper `BuildFixHint` (`src/buildfixhint.{h,cpp}`, `ants_core_lib`):

- `undeclaredSymbol(message)` → the undeclared identifier named by a
  recognised diagnostic, or `""`.
- `resolveHeader(rootCanonical, symbol)` → the project-relative header that
  declares `symbol` (reusing `SymbolQuery::findDefinition`), or `""`.

Both `cmdRecentErrors` and `cmdBuildStatus` (read path) post-process their
`errors[]` array: for each entry whose `message` names an undeclared symbol
that resolves to a project header, attach

```json
"likely_fix": { "add_include": "src/foo.h", "defines": "X", "at": "src/bar.cpp" }
```

`add_include` is the repo-relative header path (consistent with
`find_definition` file reporting). `at` is the failing file (the entry's
`file`), omitted when empty. Enrichment is best-effort: an unresolved root or
symbol simply yields no `likely_fix`, never a verb-level failure.

## Invariants

- **INV-1** — `undeclaredSymbol` extracts the identifier from all four
  recognised forms: GCC `'X' has not been declared`, GCC `'X' was not declared
  in this scope`, clang `unknown type name 'X'`, clang `use of undeclared
  identifier 'X'`. An unrelated message (e.g. `redefinition of 'X'`) yields
  `""`.
- **INV-2** — `resolveHeader` returns the header-suffixed match when the symbol
  is declared in a `.h`/`.hpp`/… file, in preference to any source-file match.
- **INV-3** — When the symbol is defined only in a source file (`foo.cpp`),
  `resolveHeader` falls back to the on-disk sibling header (`foo.h`) and returns
  `""` when no such sibling exists.
- **INV-4** — Self-gating: a symbol that resolves nowhere in the project yields
  `""` (no spurious suggestion for a typo'd local). An empty root or invalid
  symbol also yields `""`.
- **INV-5** — Wiring: both `cmdRecentErrors` and `cmdBuildStatus` reference the
  `BuildFixHint` enrichment on their diagnostics array, deduplicating repeated
  symbols and bounding the number of distinct `resolveHeader` lookups.

## Tests

`test_mcp_likely_fix.cpp` (in the `test_claude` bundle):

1. `undeclaredSymbol` across the four positive forms + a negative (INV-1).
2. `resolveHeader` against a seeded `QTemporaryDir` project: header match
   (INV-2), source-only sibling fallback + missing-sibling → `""` (INV-3),
   unresolved symbol / empty root → `""` (INV-4).
3. Source-grep of `remotecontrol.cpp` asserting both verbs invoke the
   enrichment (INV-5).

Must-fail-first: the helper is absent pre-fix, so the test fails to compile
against feature-absent code; it passes once `buildfixhint.{h,cpp}` and the two
wiring points land.
