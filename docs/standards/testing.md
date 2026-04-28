<!-- ants-test-standards: 1 -->
# Testing Standards — v1

A shareable contract for tests in this project. Pairs with the
other three standards in this folder ([coding](coding.md),
[documentation](documentation.md), [commits](commits.md)) — see
the [index](README.md) for the full set.

This standard governs ROADMAP bullets with `Kind: test`, plus the
regression-test follow-through expected for `Kind: fix`,
`audit-fix`, and `review-fix` work.

---

## 1. TDD policy — test first, code second

This project follows **test-driven development** for every code
change that ships behaviour. The cycle is:

1. **Write a failing test** that asserts the desired behaviour
   (or, for a bug fix, asserts the bug doesn't recur).
2. **Run the test** and confirm it fails on the current code.
3. **Write the minimum code** that makes the test pass.
4. **Refactor** if needed; tests stay green.
5. **Commit** code + test together (per
   [commits § 1.4](commits.md)).

This sequence catches the most common test-quality bug: a test
written *after* the fix that accidentally tests the new behaviour
without being sensitive to the old one. A test written first must
fail before the fix; otherwise the test isn't testing what you
think.

**Exceptions to TDD** (rare, must be justified in the commit
body):

- Pure refactors with no behaviour change — keep existing tests
  passing; no new test required.
- Documentation-only changes (`Kind: doc` / `doc-fix`) — no test
  needed.
- Generated code (`moc_*`, `ui_*`, etc.) — not tested directly;
  the consumer is.
- Exploratory spike / proof-of-concept clearly marked as such.

If TDD genuinely doesn't fit a change, write a comment in the
commit body explaining why so a reader understands the deliberate
deviation.

---

## 2. Principles

### 2.1 Tests test the contract, not the implementation

A test that mirrors the function's source is a regression guard
for the current implementation, not validation of correct
behaviour. Anchor tests to **external signals** wherever possible:
spec sections (RFC, ECMA, WCAG), CVE classes, contract docs,
user-visible behaviour.

Test names broadcast this:

- ✅ `test_RFC_7231_section_3_1_2_5_strips_LF`
- ✅ `test_WCAG_2_3_1_no_flashing_at_3hz`
- ❌ `test_parseHeader_branches_2_3_4`

A reviewer reading just the test names should be able to tell
which tests are validating contract vs. which are merely guarding
the current code path.

### 2.2 Verify the test fails on broken code

Even when following TDD strictly, double-check before claiming a
test locks in a fix:

```bash
git checkout HEAD~1 -- src/foo.cpp     # revert the fix
cmake --build build && ctest -R the_new_test    # must FAIL
git checkout HEAD -- src/foo.cpp       # restore the fix
ctest -R the_new_test                  # must PASS
```

If the test passes on broken code, it's not testing what you
think. Rewrite it.

### 2.3 Spec first, then test

For feature-conformance tests: write `spec.md` first as a
human-readable contract. Get user sign-off on the spec. Then
write the test that enforces each invariant.

The test references the spec by section: `// INV-3 from
spec.md § 2.1`. Reader can move between spec and test fluidly.

---

## 3. Test types

### 3.1 Unit tests

Test a single function or class in isolation. Fast (< 10 ms
each), deterministic, no I/O, no external services.

### 3.2 Feature-conformance tests

End-to-end behaviour matching its spec. Larger than unit tests
but still GUI-free where possible. Pattern:

```
tests/features/<feature_name>/
├── spec.md           # contract — human-readable invariants
└── test_<name>.cpp   # enforcement — INV-1, INV-2, … assertions
```

CMakeLists.txt wiring:

```cmake
add_executable(test_foo
    tests/features/foo/test_foo.cpp
    src/foo.cpp)
target_link_libraries(test_foo PRIVATE Qt6::Core Qt6::Test)
add_test(NAME foo_feature COMMAND test_foo)
set_tests_properties(foo_feature PROPERTIES LABELS "features;fast")
```

### 3.3 Integration tests

Cross-component tests where mocking would lose coverage. Hit a
real database / real filesystem / real subprocess where the
interaction is the thing under test.

### 3.4 Performance tests

Measure throughput / latency / memory. Tag `LABELS perf` so they
can be excluded from CI when noisy. Compare against a baseline,
not absolute thresholds, so machine differences don't fail the
test.

### 3.5 Fixture-based tests

For rule-based tools (linters, audit checks): keep `bad.cpp` and
`good.cpp` files in `tests/audit_fixtures/<rule>/`, run the rule
against them, assert N hits on `bad` and 0 on `good`. Count-based,
not line-number-based — line numbers shift across edits.

---

## 4. spec.md authoring

```markdown
# <feature> spec

**Theme:** one-line summary.

## Invariants

- **INV-1**: <observable behaviour, written as an assertion>.
  Source: RFC X.Y.Z § 4.5.
- **INV-2**: <observable behaviour>. Source: user report
  YYYY-MM-DD.
- **INV-3**: <observable behaviour>. Source: derived from INV-1
  and INV-2.

## Out of scope

What this feature explicitly does *not* do. (An empty section is
fine; the heading itself is a useful question to answer.)
```

INV numbering:

- Top-level: `INV-1`, `INV-2`, `INV-3`, …
- Sub-invariants: `INV-1a`, `INV-1b`, … (when one invariant has
  multiple sub-cases differing only in a parameter).

INVs are **append-only** within a spec. Don't renumber when
inserting — add `INV-1c` for a new sub-case after `INV-1b`. Same
policy as ROADMAP IDs.

When a spec invariant is dropped (the feature decision changed),
mark the INV as `**INV-3** (retired in 0.7.21): <reason>` rather
than deleting it — that preserves the cross-reference from old
test code and commit messages.

---

## 5. Test failure messages

A failing test must print enough to diagnose without reproducing
locally:

```cpp
QVERIFY2(grid->cellAt(0, 0).fg == QColor(255, 0, 0),
         qPrintable(QString("Cell 0,0 fg = %1, expected #FF0000")
                    .arg(grid->cellAt(0, 0).fg.name())));
```

Not just `QVERIFY(grid->cellAt(0, 0).fg == QColor(255, 0, 0))`,
which only prints "QVERIFY failed at line N".

Same principle for Python (`assert x == y, f"got {x}, want {y}"`)
and any other language: every assertion carries enough context
that the CI log alone is diagnosable.

---

## 6. Performance / determinism

- **Deterministic.** No `random.random()`, no time-of-day. If
  randomness is genuinely needed, seed it with a fixed value.
- **Fast.** Target < 100 ms each for `LABELS fast`. Move slower
  tests to `LABELS perf` or `LABELS integration`.
- **Isolated.** No shared state between tests; one failing test
  doesn't poison another.
- **No network unless opt-in.** A test that hits the network
  needs `LABELS network` and an env-var gate (e.g.
  `ANTS_TEST_NETWORK=1`).

---

## 7. Coverage policy

- **Every fix has a regression test** (per `Kind: fix`
  follow-through; TDD makes this automatic — the failing test is
  the start of the fix).
- **Every new feature has at least one feature-conformance test**
  (per `Kind: implement`).
- **Every audit / review finding has a regression test** (per
  `Kind: audit-fix` / `review-fix`).
- **Refactors don't get new tests** — they must keep the existing
  ones passing. If the refactor reveals untested behaviour, that's
  a separate `Kind: test` ROADMAP item.

---

## 8. Test commits

A test-only change uses `Kind: test` and the corresponding commit
prefix. With the `<ID>: <description>` mandate from
[commits § 1.1](commits.md):

```
ANTS-1234: lock the OSC 8 multi-row span emission

Adds INV-7c to tests/features/osc8_hyperlinks/spec.md and the
corresponding assertion in test_osc8_hyperlinks.cpp.

Co-Authored-By: …
```

When the test ships *with* a fix in the same commit (TDD's normal
case), the commit covers both — the test goes in alongside the
code change, with a single commit referencing the ROADMAP ID.

---

## 9. Anti-patterns

- ❌ Tests written *after* the fix without verifying they fail on
  pre-fix code (§2.2).
- ❌ Tests that mirror the function's source — regression guards,
  not validation.
- ❌ Mocking what should be a real integration test.
- ❌ `if (...) skip;` branches that hide platform-specific bugs.
- ❌ Tests that print "FAIL" but exit 0.
- ❌ Tests that depend on machine timing / CPU speed / FPU
  determinism.
- ❌ Tests that touch the network without an explicit opt-in flag.
- ❌ Tests with named functions like `test_works_correctly` —
  what's the *contract*?
- ❌ Tests committed in a "WIP" / failing state.
- ❌ Disabling a failing test (`@pytest.mark.skip`) without a
  ROADMAP item tracking the underlying bug.
- ❌ Skipping TDD on a behaviour change because "it's small".
