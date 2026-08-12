<!-- ants-testing-standards: 2 -->
# Testing Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/testing.md`. Read it there,
> and read `~/.claude/standards/languages/cpp.md` for the C++/CTest
> spellings.** This file carries only what is specific to *this* project.

**Why this file is a delta (2026-08-12).** It was a verbatim `/start-app`
copy, last touched 2026-04-30, and it had drifted into instructing a
**false pass**: its prove-the-test-is-real recipe rebuilt before the red run
but not before the green one, so the restoring run executed the *reverted*
binary and reported PASS. A test closed on that evidence proves nothing. The
recipe below is the corrected one and is kept here only because it is the
step this project got wrong.

## Where the rules actually live

| You want | Read |
|---|---|
| The TDD cycle, and why "watch it fail" and "watch it pass" are both required | global `testing.md` § 1 |
| Prove the test can fail | global `testing.md` § 2 |
| Test the contract, not the implementation; naming | global `testing.md` § 3 |
| Spec first, then test | global `testing.md` § 4 |
| Test types and what each is for | global `testing.md` § 5 |
| A failing test explains itself | global `testing.md` § 6 |
| Determinism, isolation, no network | global `testing.md` § 7 |
| Coverage policy | global `testing.md` § 8 |
| Tests are code | global `testing.md` § 9 |
| Anti-patterns | global `testing.md` § 10 |
| CMake/CTest wiring, `QVERIFY2`, label vocabulary | global `languages/cpp.md` |

## Project-local rules

### Proving a test is real — the corrected recipe

```bash
git stash push src/foo.cpp                    # park the fix
cmake --build build && ctest -R the_new_test  # must FAIL
git stash pop                                 # restore the fix
cmake --build build && ctest -R the_new_test  # must PASS
```

**Rebuild on both sides.** A bare `ctest` after restoring runs the binary
from the reverted build. That is a false pass and it is indistinguishable
from a real one.

**`git stash`, never `git checkout <ref> -- <file>`** — the checkout form
silently overwrites uncommitted edits in that file.

**Check the matched count, not the pass rate.** `ctest -R` filters test
*names*, not build targets, and it is **case-sensitive**: a pattern that
matches none of your tests still runs whatever else it happens to match and
reports "100% tests passed". Run `ctest -N -R <pattern>` first and confirm
the number is the one you expect.

**Build the right target.** Feature tests compile into shared bundles, not
standalone executables, and the bundle is not guessable from the path —
`tests/features/spec_conformance/` builds into `test_claude`. Building the
wrong target succeeds silently and runs the old binary, so the new test
neither compiles nor appears. `grep -n <feature> CMakeLists.txt`, then read
upward for the enclosing `ants_add_*_bundle(`.

### Audit rule fixtures

`tests/audit_self_test.sh` matches rule regexes against
`tests/audit_fixtures/<rule>/{bad,good}.*`. The `bad` file carries N hits
each marked `// @expect <rule-id>`; the `good` file must produce zero. The
harness is **count-based, not line-based**, so a fixture may move lines
freely.

### Labels actually in use

`features`, `fast`, `perf`, `e2e`, `audit`. Set them with
`set_tests_properties(... LABELS ...)`.

`e2e` and `perf` are excluded from the default presets — `e2e` drives a
throwaway GUI instance, and `perf` benchmarks must not contend under `-j`.

> Corrected 2026-08-12: this file previously documented `integration` and
> `network` labels that no test uses, and omitted `e2e` and `audit`, which
> several do. It also documented an `ANTS_TEST_NETWORK=1` opt-in gate that
> **does not exist** — the string occurred nowhere in the repo but in this
> sentence. There is no network-gated test today; if one is added, add the
> gate and document it here at the same time.

### Timing budgets

A unit test should run in well under 10 ms and anything labelled `fast`
under ~100 ms. These are budgets for keeping the suite usable at `-j4`, not
assertions — do not write a test that fails on a slow machine because of
them. Global `testing.md` § 5's warning about machine-specific thresholds
applies to anything you are tempted to assert on.

## What checks this

The suite itself (`ctest --preset=default`, ~19 s at `-j4`) and the pre-push
hook, which runs the correctness labels against `build/` **without
building** — so a stale `build/` gates on stale binaries. `tools/ci-parity.sh
--full` is the complete CI mirror. Nothing checks the recipe above; it is
read.
