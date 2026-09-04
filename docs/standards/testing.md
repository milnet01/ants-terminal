<!-- ants-testing-standards: 3 -->
# Testing Standards — Ants Terminal deltas

> **The standard itself is `~/.claude/standards/testing.md`.**
> `~/.claude/standards/languages/cpp.md` carries the C++ idioms, but **three
> of its testing rules do not apply to this project** — see the note under the
> table. This half of the file carries only what is specific to *this*
> project.
>
> **The owner is mirrored verbatim below the divider**, between the
> `MIRROR BEGIN` / `MIRROR END` markers, because this repo is public and an
> outside reader cannot open a path inside a private home directory. **Do not
> edit that half.** A correction goes upstream, then
> `tools/check-standard-mirrors.sh --write` re-copies it down;
> `tools/hooks/pre-commit` refuses a commit whose mirror has drifted from its
> owner (ANTS-4133). Note that `languages/cpp.md` is **not** mirrored — only
> the four deltas' own owners plus `security.md` are.

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

**A mutation harness must BUST THE MTIME when it restores a file.** Ninja
decides what to rebuild from timestamps, so restoring a mutated source by a
method that *preserves* the old mtime — Python's `shutil.copy2`, `cp -p`,
`rsync -t`, restoring from a tar — leaves ninja believing the object file is
current. It skips the rebuild, the mutation survives into a binary that links
and runs green, and the must-fail-first proof above is vacuous: the run you
read as "the fix restored it" never compiled the restored file. Worse, the
mutations then ACCUMULATE across iterations of a sweep.

Write the bytes back (Python `Path.write_text`, a plain `cp` with no `-p`, a
shell redirect) rather than copying with attributes preserved. The `git
revert` / `git stash` recipes above are already safe — git stamps a fresh
mtime — so this fires on hand-rolled harnesses, which is where it has bitten.

Recorded here rather than in the global standard because the mechanism is
ninja's, and because it had no home at all: ANTS-3793 § 6 and ANTS-3808 § 6
both cited `testing.md` for it while the word `mtime` appeared nowhere in the
file (ANTS-3826).

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

**The mirrored half is checked.** `tools/hooks/pre-commit` runs
`tools/check-standard-mirrors.sh`, which fails the commit if the text between
the MIRROR markers no longer matches `~/.claude/standards/testing.md`. It
skips on a checkout with no global standards tree — an outside contributor's,
or CI's — since there is then nothing to compare against.

## Review loop log

| Loop | Date | Lanes | Q1/Q2/Q3/Q4 | Outcome |
|---|---|---|---|---|
| 1 | 2026-08-12 | 1 (cold, general-purpose) | Q1 2 · Q2 3 · Q3 1 · Q4 n/a | 6 verified, 6 fixed, 0 dismissed. **The draft's own fix was broken.** `git stash push` on a *committed* fix — the case the recipe exists for — prints `No local changes to save`, exits 0 and leaves the fix in place, so the "must FAIL" run passes; the paired `pop` then lands an unrelated stash entry. Verified in a throwaway repo and replaced with `git revert --no-commit` / `--abort`, round-tripped before being written down. Also: the zero-match ctest message is `No tests were found!!!` with exit 0, not "100% tests passed"; `LABELS` replaces rather than adds; the cpp.md pointer routed to an uncorrected recipe, the wrong labels and `add_executable` wiring; `debug` excludes only `e2e`. |

---

<!-- MIRROR BEGIN ~/.claude/standards/testing.md -->
# Testing Standards — v1

**Rule history: [`docs/history/testing.md`](../docs/history/testing.md).**

**Status:** v1 (2026-08-08).

**Purpose: so that a change which breaks behaviour fails a test before it
reaches a user.**

Every rule here traces to that sentence. It is a narrow purpose on
purpose: a test suite that is thorough, elegant and never fails when
something breaks has done nothing.

**Concepts only.** How a test is written, labelled and run in a given
language lives in `languages/<name>.md` — the same split as `coding.md`.
Governs **every change that ships behaviour, whatever its `Kind`** — §1's
test-first order and §8's conformance test bind an `implement` or `feature`
item exactly as they bind a `test` one.
The regression-test follow-through specifically covers `fix`,
`audit-fix` and `review-fix`.

---

## 1. Test first, code second

For every change that ships behaviour:

1. **Write the test**, asserting the desired behaviour — or, for a bug,
   asserting the bug does not recur.
2. **Run it, and watch it fail.** You know it should fail, because the
   code that satisfies it does not exist yet.
3. **Write the code.**
4. **Run the test again, and watch it turn green.**
5. **Refactor** if needed, tests staying green.
6. **Commit code and test together.**

**Steps 2 and 4 are the point, and step 2 is the one people skip.** Seen
failing and then seen passing, with only the code changing between, is
what proves the test is sensitive to the thing it claims to test.

A test written after the fix passes the first time it is ever run. That
tells you nothing: it may be asserting something the old code also
satisfied. **A test never observed failing is not known to test
anything** — which is why the order is the rule, not merely the presence
of a test.

**Exceptions**, each stated in the commit body so a reader can tell a
decision from an omission: a pure refactor with no behaviour change; a
documentation-only change; generated code, where the consumer is what is
tested; a clearly-marked exploratory spike.

## 2. Proving a test that arrived late

**Follow §1 and there is nothing to do here** — you watched it fail at
step 2, and that *is* the proof.

This section is for the other case: a test that already exists alongside
the code it tests, so nobody has seen it fail. A test written after its
fix, one inherited with a codebase, one added during a review.

For those, reproduce §1's evidence backwards: **remove the fix, run the
test and see it fail; restore the fix, run it and see it pass.** Only the
code changes between the two runs. `languages/<name>.md` has the
commands.

If it passes against the broken code it is not testing what you think,
and rewriting it is the only remedy.

**The temptation to skip this is strongest exactly where it matters
most** — closing a bug, where the fix is already in the tree and the test
is already green, so everything looks finished.

## 3. Test the contract, not the implementation

A test that mirrors the function's source is a regression guard for
today's code, not a check that the behaviour is right. It will fail on
every refactor and pass on every misunderstanding.

**Anchor to something external wherever one exists** — a published
specification, a standards clause, a security class, the project's own
contract document, or observable user behaviour.

**Let the test's name carry that anchor**, so a reviewer reading only the
names can tell which tests validate a contract and which merely guard a
code path. A name that says what the test *proves* survives a refactor; a
name that says which branches it walks does not.

A name like `test_works_correctly` states nothing at all — the useful
question it dodges is *what contract?*

## 4. Contract first, then the test

For feature-conformance work, write the contract before the test: a short
human-readable document listing the invariants, each one an observable
behaviour with its source.

The test then references the contract by invariant id, so a reader can
move between the two without guessing which assertion covers what.

**Invariant ids take `spec-format.md` §3.7's `INV-N` form, and are
append-only.** They are cited from commit messages, changelog entries and
sibling documents, so renumbering silently breaks references that nothing
checks. A dropped invariant is marked **withdrawn**, with the version and
reason, rather than deleted.

That word is [spec-format.md](spec-format.md) §3.7's, which owns the
Invariants section the marking lives in — this section said *retired*
until 2026-08-14, so the two standards named one state two ways, shared
no searchable token, and a search for dead invariants found half of them
(ROADMAP CFG-0098). *Retired* keeps its separate meaning elsewhere: a
rule or a skill that has been dropped.

## 5. Kinds of test

| Kind | What it covers | Cost |
|---|---|---|
| **Unit** | one function or class, isolated — no I/O, no services | fast enough to run constantly |
| **Feature conformance** | one behaviour end to end, against its contract | slower, still headless where possible |
| **Integration** | a real interaction where mocking would remove the thing under test | slow, and worth it precisely there |
| **Performance** | throughput, latency, memory, against a **baseline** rather than an absolute number — a machine-specific threshold fails on a different machine, which teaches everyone to ignore it | noisy |
| **Fixture-based** | a rule-based tool, run against known-good and known-bad inputs, asserting **counts** rather than line numbers, which shift on every edit | fast |

**Label them so the slow ones can be excluded**, and so a developer can
run the fast set without deciding what to skip. A suite that is
inconvenient to run does not get run, and then it fails the purpose.

## 6. A failing test explains itself

**Every assertion prints enough to diagnose from the log alone** —
what was expected, what was received, and enough context to place it. A
failure message that names only a file and a line number sends whoever
reads it back to reproduce something that has already happened.

This matters most where reproduction is hardest: a failure on someone
else's machine, in CI, or intermittently. Those are exactly the failures
a bare assertion tells you nothing about.

`languages/<name>.md` has the spelling for each language.

## 7. Determinism

A test that fails sometimes is worse than no test: it trains everyone to
re-run rather than investigate, and it hides the real failure among the
noise.

- **No unseeded randomness**, and no dependence on the time of day.
- **No dependence on timing, machine speed or execution order** — outside
  a labelled performance test (§5), whose measurement is exactly that.
- **Isolated** — no shared state, so one test's failure cannot cause
  another's.
- **No network unless explicitly opted in**, with a label and a gate,
  because a test that fails when the connection drops is not testing your
  code.

**A test disabled to stop it failing is a bug with the alarm switched
off.** Disabling one requires a tracked item for the underlying problem;
otherwise the suite quietly stops covering what everyone assumes it
covers.

## 8. Coverage

- **Every fix carries a regression test.** Test-first makes this
  automatic — the failing test *is* the start of the fix.
- **Every new feature carries at least one conformance test** against
  its contract.
- **Every audit or review finding that gets fixed carries one**, for the
  same reason: a finding fixed without a test is one that can return
  unnoticed.
- **Refactors get no new tests.** They must keep the existing ones
  passing; that is what makes them refactors. If a refactor reveals
  untested behaviour, that is its own tracked item.

**Coverage percentage is not a target.** It measures which lines ran, not
whether anything was checked — a suite can execute every line and assert
nothing. Ask instead whether the behaviours that matter would fail if
broken.

## 9. Tests are code

`coding.md` applies to them: named for what they mean, no dead branches,
no cleverness that costs legibility.

The one place tests differ: **`coding.md` §1.3 does not apply inside a
test body** — neither its reuse ladder nor the Rule of Three, so
duplication between tests needs no commit-body justification. Extract
only where the duplicated block is itself the thing under test, or where a
change to it would have to be made identically in every copy to stay
correct — never merely because it repeats. **A little duplication in a test is better
than a helper**, because a test should be readable in one place without
following a chain of abstractions to find out what it actually asserts.

## 10. Anti-patterns

- ❌ A test written after the fix and never seen failing.
- ❌ A test that mirrors the implementation it is testing.
- ❌ Mocking the very interaction the test exists to cover.
- ❌ A skip branch that hides a platform-specific failure.
- ❌ A test that reports failure and exits successfully.
- ❌ Dependence on timing, machine speed or test order, outside a labelled
  performance test.
- ❌ Touching the network without an explicit opt-in.
- ❌ A test named for what it walks rather than what it proves.
- ❌ Committing a test in a failing or unfinished state.
- ❌ Disabling a failing test with nothing tracking the cause.
- ❌ Skipping the order because the change is small.
- ❌ Chasing a coverage number.

## What checks this

| Rule | What catches a breach |
|---|---|
| Tests pass (§1) | the test runner — **`Partial:`** locally always, and in CI only where the project has a pipeline that runs it. §1 states no CI requirement, so on a project without one nothing checks this except the person who remembers to run it |
| Determinism (§7) | repeated runs, and a shuffled run order where the runner supports it |
| Network isolation (§7) | running the fast set with no connection |
| Speed labels honoured (§5) | the runner's own timing report |
| **The test was seen failing before the code existed (§1)** | **nothing mechanical** — once both are green, nothing distinguishes a test written first from one written after |
| Tests the contract, not the implementation (§3) | **nothing mechanical** — a reader, helped by the naming rule |
| Every fix has a regression test (§8) | **nothing mechanical** — visible in review as a fix commit with no test beside it |
| A disabled test has a tracked cause (§7) | skip markers are greppable; whether the tracked item is real is not |

**The first `nothing` row is the largest hole in this standard**, and it
is unclosable by tooling: the whole discipline of §1 and §2 rests on
something no artifact records. That is why both sections state *why* the
order matters rather than merely requiring it — a rule only habit
enforces has to be understood to survive.

## Cold-eyes loop log

Rows live in [`docs/reviews/testing-loop-log.md`](../docs/reviews/testing-loop-log.md).
`documentation.md` § 9.1 owns the form.
<!-- MIRROR END -->
