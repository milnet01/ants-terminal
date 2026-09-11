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

## ANTS-5062 — the walk descended before excluding anything

`walkTestFiles` matched the exclusion regex against the candidate's
**absolute** path, and walked with `QDirIterator::Subdirectories` before
deciding anything was excluded, once per glob. A project rooted under a
directory that happens to be named `build` had every file inside it
excluded, because the project root's own path satisfied the pattern — not
because anything under it was build output.

- **INV-6 / exclusion matches the relative path.** The exclusion regex is
  matched against the path relative to the walk, not the absolute path.
  A project whose root sits under a directory literally named `build`
  still returns its own test files.
- **INV-7 / routes through the shared pruning walk.** Neither
  `walkTestFiles` (`src/testauditengine.cpp`) nor the review engine's
  whole-tree walks (`src/indiereviewengine.cpp`) contain
  `QDirIterator::Subdirectories` — a walk that lists every file before
  excluding it. Both contain a call to `PrunedWalk::walkFiles(`, the
  primitive that decides at the directory, before recursing into it.
