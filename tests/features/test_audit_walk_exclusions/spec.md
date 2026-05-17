# Feature spec: ANTS-1451 — `walkTestFiles` build-tree exclusions

The test-audit MCP trio walks the project tree for test files via
`TestAuditEngine::internal::walkTestFiles`. On 2026-05-17, the live
Ants Terminal run returned 419 files with chunk c-001 entirely
populated by `build-asan/.../moc_*.cpp` + `mocs_compilation.cpp` —
the exclusion list only filtered the literal `/build/` segment, so
preset build trees (`build-asan/`, `build-workstation/`) and ctest
autogen subtrees (`_deps/`, `CMakeFiles/`, `autogen/`) slipped
through.

## Invariants exercised by this test

- **INV-1 / `/build*/` glob excluded.** `build/`, `build-asan/`,
  `build-workstation/`, `build-debug/`, any future `build-*` preset
  must be skipped — not just the bare `build/` segment.
- **INV-2 / CMake autogen subtrees excluded.** `_deps/`,
  `CMakeFiles/`, `autogen/` segments must be skipped wherever
  they appear in the path.
- **INV-3 / real test files still surface.** Files under `tests/`
  (or basename-matching `test_*.cpp`) anywhere in the tree are
  returned. The exclusion list is build-tree-only and does not
  shadow project tests.
- **INV-4 / pre-existing exclusions retained.** `/node_modules/`,
  `/.venv/`, `/__pycache__/`, `/dist/` remain in the exclusion set.
  Regression guard so the rule consolidation didn't drop coverage.
- **INV-5 / single source of truth.** `walkTestFiles` uses one
  compiled `QRegularExpression` for the full exclusion set; no
  hand-rolled `if … || … || …` chain reintroduced.
