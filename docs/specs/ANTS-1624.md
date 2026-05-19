# ANTS-1624 — `test_audit_partition` libcheck C-unit-test detection

**Status:** implementing
**Kind:** enhancement
**Source:** cross-session-report-2026-05-18 (RetroArch Bundle 70 — `/test-audit`
on libretro-common's libcheck suite)
**Pull:** 25 (MCP test-audit framework + last_audit_summary picker fold-in)
**Composes with:** [[ANTS-1623]] (polyglot framework detection — same probe
table; libcheck lands as a fourth-row entry before that work makes the table
per-tree); [[ANTS-1455]] (glob walker — libcheck globs ride the same path).

## 1. Problem

`test_audit_partition`'s framework probe table at `testauditengine.cpp:78`
ships five entries: pytest / jest / ctest / cargo / go. RetroArch Bundle 70
(2026-05-18, `/test-audit` against `libretro-common/test/`) dispatched on a
five-file libcheck suite — `test_hash.c`, `test_lists.c`, `test_queues.c`,
`test_string.c`, `test_utils.c` — and got `code: "no_tests_found"`. Each
file `#include <check.h>`, defines `START_TEST` / `END_TEST` /
`Suite *create_suite()`, and is registered in `Makefile.test`. Total LoC
2274.

The miss isn't a corner case: libcheck is the standard C-unit-test runner
on Linux/BSD, ships in Fedora/openSUSE base, and is used across libretro-
common (which is shared by dozens of downstream cores per upstream). Every
project that uses libcheck silently fails partition today; the workaround
is manually dispatching `Agent(test-audit-chunk)` with hand-rolled chunks,
which loses the pre-pass findings + structured fenced bundle.

## 2. Fix

Add a sixth probe-table entry, ordered between `jest` and `ctest`:

```c++
{QStringLiteral("libcheck"),
 {QStringLiteral("Makefile.test")},
 {QStringLiteral("tests/**/test_*.c"),
  QStringLiteral("**/test_*.c"),
  QStringLiteral("**/tst_*.c")}},
```

### 2.1 Why `Makefile.test` is the signal file

The natural signal — `#include <check.h>` inside test sources — would need a
content scan over candidate `.c` files before the probe table runs. That's a
larger surface change and would slow probe latency on every project (even
those without libcheck). `Makefile.test` matches the existing probe model
(file-existence check, O(1) `QFileInfo::exists`) and catches every project
in the cross-session-reports trail: libretro-common uses `Makefile.test`,
upstream libcheck's own build system uses it, and Fedora/openSUSE libcheck
demos all ship it. False-positive risk is low because `Makefile.test` is
not a common name outside libcheck-style suites.

### 2.2 Why before ctest

Probe order is first-match-wins. `Makefile.test` is more specific than
`CMakeLists.txt` — almost every C/C++ project has `CMakeLists.txt`, very
few have `Makefile.test`. Ordering libcheck **before** ctest means a project
that uses both build systems (CMake for the main app, libcheck for tests)
gets the C-unit detection it actually has. The reverse order would route
every CMake project to `ctest` (whose globs are `tests/**/*.cpp`,
`test_*.cpp` — `.cpp` only) and miss the `.c` test sources.

### 2.3 Glob choice

Three globs cover the common shapes:

- `tests/**/test_*.c` — canonical layout (libretro-common, GNU coreutils).
- `**/test_*.c` — projects that scatter test files alongside source.
- `**/tst_*.c` — older convention (Mesa, glibc).

The walker's existing exclusion list (`node_modules`, `.venv`,
`__pycache__`, `build*`, `dist`, `_deps`, `CMakeFiles`, `autogen`) already
keeps these out of vendor trees.

## 3. Invariants

- **INV-1** (probe-table entry exists). `g_kFrameworks()` in
  `testauditengine.cpp` contains an entry with `name == "libcheck"`,
  `signalFiles == ["Makefile.test"]`, and `testGlobs` including
  `tests/**/test_*.c`. Source-grep guard.
- **INV-2** (probe order — libcheck before ctest). The `libcheck` entry
  appears before the `ctest` entry in `g_kFrameworks()`. Source-grep
  for `"libcheck"` index < `"ctest"` index in the returned vector.
- **INV-3** (detection — bare Makefile.test). A project tree with
  `Makefile.test` at root + one `tests/test_hash.c` ⇒ `partition()`
  returns `framework == "libcheck"`, `totalFiles == 1`, `ok == true`.
  Runtime test against a `QTemporaryDir`.
- **INV-4** (detection — libretro-common-style). A project tree with
  `Makefile.test` at root + five `libretro-common/test/<sub>/test_<n>.c`
  ⇒ `partition()` returns `framework == "libcheck"` and `totalFiles == 5`.
- **INV-5** (CMakeLists.txt + Makefile.test → libcheck wins). When both
  files exist at project root, `detectFramework` returns `"libcheck"`,
  not `"ctest"`. Guards the probe-order INV-2 at the behavioural level.
- **INV-6** (no false positive on pure-CMake). A project tree with
  `CMakeLists.txt` but no `Makefile.test` ⇒ `framework == "ctest"`
  unchanged. Backwards-compat guard.

## 4. Test

Extend `tests/features/mcp_test_audit_trio/test_mcp_test_audit_trio.cpp`:

- `Ants1624LibcheckEntryPresent` (INV-1) — source-grep
  `src/testauditengine.cpp` for `"libcheck"` + `"Makefile.test"` +
  `"tests/**/test_*.c"`.
- `Ants1624LibcheckBeforeCtest` (INV-2) — source-grep for the literal
  positions; assert the `libcheck` line number < `ctest` line number.
- `Ants1624BareMakefileTest` (INV-3) — runtime; `QTemporaryDir` +
  `Makefile.test` + `tests/test_hash.c` + `partition()` call.
- `Ants1624LibretroCommonShape` (INV-4) — runtime; same temp dir
  shape, five test files in `libretro-common/test/<sub>/`.
- `Ants1624CMakeAndMakefileTestPrefersLibcheck` (INV-5) — runtime;
  temp dir with both `Makefile.test` and `CMakeLists.txt` + one C test.
- `Ants1624PureCMakeStillCtest` (INV-6) — runtime; temp dir with
  `CMakeLists.txt` only + one `tests/test_x.cpp`, assert framework is
  `"ctest"` (backwards-compat).

All tests GUI-free; link `testauditengine.cpp` + `pathvalidation.cpp` +
the engine's deps. Label `features;fast`.

## 5. Out of scope (deferred)

- **`#include <check.h>` content-scan detection.** Defers cost to every
  partition call. Punt to ANTS-1623 (polyglot per-tree detection)
  which will rework the probe model anyway.
- **`Makefile.am` / `Makefile.in` heuristics.** Autotools-style projects
  hide the libcheck dependency inside `configure.ac` (`AC_CHECK_LIB`).
  Out-of-scope until a cross-session report shows the gap on a real
  project — the `Makefile.test` signal already covers libretro-common,
  GNU coreutils, and the upstream libcheck demos.
- **Error-envelope hint.** ANTS-1624's body suggested naming the probed
  framework set in the `no_tests_found` envelope so callers see what
  was looked for. Deferred to a follow-up — landing the detector is
  the load-bearing fix here; the hint is a UX polish atop.

## 6. RAM / build budget

- Probe table: one new 3-string `FrameworkProbe` entry. ~64 bytes of
  static const. No new allocations per `partition()` call (the table
  is `static const`).
- Detection: one extra `QFileInfo::exists` probe per `detectFramework`
  call when neither pytest nor jest signals match. Constant overhead.
- Walker: globs flow through the existing `globToRegex` + `walkTestFiles`
  pipeline; no new code path. Recursion bounded by the existing
  exclusion list (build*, node_modules, etc.).
- Test fixtures: ≤ 5 temp files per runtime case; auto-cleaned by
  `QTemporaryDir`.

## 7. Sources

- RetroArch cross-session-report 2026-05-18 — Bundle 70 (libretro-common
  libcheck suite, 5 test files, 2274 LoC, `no_tests_found` return).
- [[ANTS-1455]] — glob-to-regex walker (host for the new globs).
- [[ANTS-1623]] — polyglot per-tree detection (composes; libcheck
  lands as a normal probe-table entry first).
- [[ANTS-1397]] — `test_audit_*` trio v1 spec (parent).
