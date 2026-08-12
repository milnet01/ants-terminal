<!-- ants-testing-standards: 2 -->
# Testing Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/testing.md`. Read it there.**
> `~/.claude/standards/languages/cpp.md` carries the C++ idioms, but **three
> of its testing rules do not apply to this project** — see the note under the
> table. This file carries only what is specific to *this* project.

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
| `QVERIFY2` and the C++ idioms | global `languages/cpp.md` |

**Three things in `languages/cpp.md` do NOT apply here, and this file wins on
all three:** its prove-the-test recipe omits the rebuild before the green run;
its label vocabulary (`integration`, `network`) is not this project's; and its
feature-test wiring is `add_executable`, where this project uses shared
bundles. Sections below own each.

## Project-local rules

### Proving a test is real — the corrected recipe

**The fix is usually already committed** — a test written after its fix, or
one added during review. Those two cases need different commands.

Fix already committed:

```bash
git revert --no-commit <fix-sha>              # un-apply the fix
cmake --build build && ctest -R the_new_test  # must FAIL
git revert --abort                            # restore it
cmake --build build && ctest -R the_new_test  # must PASS
```

Fix still uncommitted in the working tree:

```bash
git stash push src/foo.cpp                    # park the fix
cmake --build build && ctest -R the_new_test  # must FAIL
git stash pop                                 # restore it
cmake --build build && ctest -R the_new_test  # must PASS
```

**Do not use `git stash` on a committed fix.** `git stash push <path>` with
nothing modified prints `No local changes to save` and **exits 0**, leaving
the fix in place — so the "must FAIL" run passes and you conclude the *test*
is wrong. The paired `git stash pop` then pops an unrelated stash entry into
your tree. Both verified 2026-08-12.

**Do not use `git checkout <ref> -- <file>` for either** — it silently
overwrites uncommitted edits in that file.

**Rebuild on both sides.** A bare `ctest` after restoring runs the binary
from the reverted build. That is a false pass and it is indistinguishable
from a real one. Global `languages/cpp.md`'s version of this recipe omits the
second rebuild; the recipe above is the corrected one and wins here.

**Neither a green line nor a zero exit status proves your test ran.** `ctest
-R` filters test *names*, not build targets, and it is **case-sensitive**
(`-R roadmap` matches 367 tests, `-R Roadmap` a disjoint 348). A pattern
matching *other* tests reports "100% tests passed"; one matching **nothing**
prints `No tests were found!!!` and **still exits 0**, so a scripted
`ctest -R … && …` chain treats it as success. Run `ctest -N -R <pattern>`
first and confirm the count is the one you expect.

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

**`LABELS` REPLACES; it does not add.** On a bundle test it overwrites the
bundle's `features;fast`, so the case silently leaves the default presets and
the pre-push gate. Restate every label you still want.
`tests/slow_test_timeouts.cmake` documents this and keeps a named drift guard
(`RoadmapReadSeam.Ants3793LatencyCaseIsPerfLabelled`) against it.

`e2e` and `perf` are excluded from the `default`, `fast` and `workstation`
presets — `e2e` drives a throwaway GUI instance, and `perf` benchmarks must
not contend under `-j`. **`debug` excludes only `e2e`**, so a `perf` case does
run there, under ASan.

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

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 2 · Q2 3 · Q3 1 · Q4 n/a | 6 verified, 6 fixed, 0 dismissed. **The draft's own fix was broken.** `git stash push` on a *committed* fix — the case the recipe exists for — prints `No local changes to save`, exits 0 and leaves the fix in place, so the "must FAIL" run passes; the paired `pop` then lands an unrelated stash entry. Verified in a throwaway repo and replaced with `git revert --no-commit` / `--abort`, round-tripped before being written down. Also: the zero-match ctest message is `No tests were found!!!` with exit 0, not "100% tests passed"; `LABELS` replaces rather than adds; the cpp.md pointer routed to an uncorrected recipe, the wrong labels and `add_executable` wiring; `debug` excludes only `e2e`. |
