# test_audit_libcheck_detection — feature-conformance test

Anchors **[ANTS-1624]** — libcheck C-unit-test framework detection in
`test_audit_partition`. Asserts spec INVs 1, 2, 3, 4, 5, 6.

## INVs

- **INV-1 (probe-table entry exists)** — `g_kFrameworks()` carries a
  libcheck entry with signal file `Makefile.test` and `.c` test globs.
  Source-grep on `src/testauditengine.cpp`.
- **INV-2 (probe order — libcheck before ctest)** — the libcheck entry
  appears before the ctest entry in the table. Source-grep on the line
  positions.
- **INV-3 (bare Makefile.test detection)** — `QTemporaryDir` with
  `Makefile.test` at root + `tests/test_hash.c` ⇒ `partition()` returns
  `framework == "libcheck"`, `totalFiles == 1`.
- **INV-4 (libretro-common-style layout)** — `Makefile.test` at root +
  five `libretro-common/test/<sub>/test_<name>.c` files ⇒ all five
  surface.
- **INV-5 (CMake+Makefile.test → libcheck wins)** — both signal files
  present, plus one `tests/test_x.c` ⇒ framework is `"libcheck"`, not
  `"ctest"`. Probe-order behavioural guard.
- **INV-6 (pure-CMake unchanged)** — `CMakeLists.txt` only + one
  `tests/test_x.cpp` ⇒ framework is `"ctest"`. Backwards-compat.

## Tools used

- `TestAuditEngine::partition` — direct engine call (no MCP layer).
- `QTemporaryDir` — fixture trees.
- `SRC_TESTAUDITENGINE_CPP_PATH` — source-grep tripwires.

## Source

cross-session-report-2026-05-18 — RetroArch Bundle 70 (libretro-common's
libcheck suite, 5 test files, 2274 LoC, hit `no_tests_found` because the
framework probe didn't recognise `Makefile.test`).
