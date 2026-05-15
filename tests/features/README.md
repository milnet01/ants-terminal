# Feature conformance tests

This directory holds **behavioral contract tests** — one subdirectory per
feature, each pairing a human-readable spec (`spec.md`) with a runnable
executable that exercises the feature through its real API and asserts
the observable output matches the contract.

This complements, rather than replaces, the unit-style shell tests in
`tests/audit_fixtures/`. The difference:

| Style | Tests what | Format | Example |
|---|---|---|---|
| `audit_fixtures/` | one regex matches the right fixture files | shell + fixture files | `audit_rule_fixtures` |
| `features/` | a feature's observable behavior satisfies a written contract | `spec.md` + C++ test exec | `scrollback_redraw` |

## Layout

```
tests/features/
  <feature-name>/
    spec.md                 # human contract — reviewed like code
    test_<feature>.cpp      # GoogleTest TEST() blocks under one Suite
```

## Bundles (ANTS-1217)

Test binaries are consolidated by subsystem to bound build-time RAM.
Each `test_*.cpp` registers `TEST(SuiteName, FeatureName)` blocks against
shared bundle binaries; `gtest_discover_tests` turns each TEST block into
its own ctest entry, so `ctest -L features` and parallel `ctest -j` keep
working unchanged.

| Bundle | Subsystem | Status |
|---|---|---|
| `test_vt`      | vtparser + terminalgrid + themes  | shipped |
| `test_chrome`  | mainwindow + tabs + palette + status bar + menu + dialogs hosted in chrome | shipped |
| `test_claude`  | Claude integration + allowlist + AI + SSH + shell-utils | shipped |
| `test_audit`   | audit dialog + engine + hygiene + rule-quality + command-trust + feature-coverage | shipped |
| `test_dialogs` | settings + roadmap + diff (dialog subjects hosted by chrome) | shipped |
| `test_lua`     | luaengine + pluginmanager (gated `LUA_FOUND`) | shipped |
| `test_core`    | session + pty + remote-control + config + debuglog + threading + packaging | shipped |

### Suite-name convention

**Each subdirectory owns a unique Suite name** derived from the directory
name in CamelCase: `scrollback_redraw → ScrollbackRedraw`,
`decstr_soft_reset → DecstrSoftReset`. Within a Suite, the TEST name is
the assertion topic (`OriginMode`, `AutoWrap`, `ScrollRegion`, …).

Two test files in the same directory share a Suite — give their TEST
names a disambiguating prefix (`test_redraw.cpp` uses
`IdenticalRepaint2J`, `DivergedRepaint2J`, …; `test_viewport_stable.cpp`
prefixes its TEST names with `ViewportStable…`).

### ctest filter syntax

```bash
# Run all tests in the vt bundle:
ctest --test-dir build -L features -R 'ScrollbackRedraw|DecstrSoftReset|VtparserSimdScan'

# Run a single TEST block:
ctest --test-dir build -R 'DecstrSoftReset\.OriginMode'

# Run all tests in one Suite (one subdir's worth):
ctest --test-dir build -R 'DecstrSoftReset\.'
```

Note the literal dot escaped (`\.`) — ctest treats the argument as a
regex and unescaped `.` matches any character.

## Why this matters

Unit tests verify code paths. Feature tests verify user-observable
behavior matches the written contract. When a change silently alters
behavior (e.g. a partial fix that only covers one code path), unit
tests keep passing but the feature regresses. A feature test with a
clear invariant catches that gap.

The first feature test in this directory — `scrollback_redraw` —
exists specifically because 0.6.21 shipped an incomplete fix for the
main-screen TUI scrollback-doubling bug. The 0.6.21 fix covered the
"user scrolled up" path but not the "user at the bottom" path, which
is the common case. A feature test asserting the top-level invariant
("scrollback must not double after CSI 2J repaint of the same content")
would have caught the gap at commit time instead of after the bug was
reported against three prior releases.

## Authoring a new feature test

1. `mkdir tests/features/<feature-name>/`.
2. Write `spec.md`: the feature's invariants, in plain English,
   reviewable by humans. Include motivation, scope, and what's *out*
   of scope.
3. Write `test_<feature>.cpp`: exercise the feature through its real
   public API. Use `TEST(SuiteName, ...)` blocks (Suite name = the
   directory name in CamelCase). Assert invariants with `EXPECT_*` /
   `ASSERT_*`. Stream extra context via `<<` on failure.

   **For labelled invariant checks (`INV-1`, `INV-2` …) use the
   shared helper at `tests/_support/expect.h`** (ANTS-1382):

   ```cpp
   #include "../../_support/expect.h"
   ANTS_TEST_SCOPE();   // at file scope, outside any namespace

   TEST(MyFeature, Main) {
       expect_reset();
       expect(condA,  "INV-1/condA");
       expect(condB,  "INV-2/condB", QStringLiteral("got %1").arg(x));
       ASSERT_EQ(0, expect_finish());
   }
   ```

   PASS labels are silently counted; on the first FAIL the helper
   flushes a single `(N prior ok)` summary plus the FAIL line —
   `ctest --output-on-failure` tails stay short. The macro emits
   `expect()` overloads for `const char*`, `QString`, and
   `std::string` details. Per-TU scope, so multiple test files in
   one bundle don't share state.
4. Add the source path to the appropriate bundle's `SOURCES` list in
   `CMakeLists.txt` — `test_vt` for vtparser/terminalgrid features,
   `test_chrome` for mainwindow/tabs, etc. Do **not** add a new
   `add_executable` unless the test legitimately needs process
   isolation (death tests, signal handlers — see Risks in
   `docs/specs/ANTS-1217.md`).
5. Run `ctest -L features` and confirm it passes.
6. Run it against the pre-fix code (temporarily revert the fix) to
   confirm it *would* have caught the bug. If it doesn't fail
   pre-fix, the invariant is too loose — tighten it.

The "run against pre-fix to confirm the test would have caught it"
step is non-negotiable. A test that passes on broken code has no
value.

## What goes here vs. unit tests

Belongs here:
- VT protocol conformance (escape → grid state pairs)
- Scrollback / reflow invariants
- OSC 8 / OSC 52 / OSC 133 behavioral contracts
- Session save/restore round-trip invariants
- Audit dialog rule execution end-to-end

Belongs in `audit_fixtures/` or inline unit tests:
- Regex matches expected fixture files
- Pure-function algorithmic correctness (UTF-8 decode, glob-to-regex)
- Data-structure invariants

Rule of thumb: if the invariant makes sense to a user and survives
implementation rewrites, it belongs here. If it only makes sense to
someone reading the code, it belongs elsewhere.
