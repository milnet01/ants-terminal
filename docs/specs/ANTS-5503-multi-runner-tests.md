<!-- ants-spec-format: 1 -->
# ANTS-5503 — Teach `test_results` and `focused_test` pytest, jest, vitest, cargo and go

**Status:** accepted (2026-10-21).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5503 (claude-config joint review 2026-09-27, A2).
**Pairs with:** ANTS-1300 (`test_results`), ANTS-1302 (`focused_test`).
**Composes with:** ANTS-5498 (nothing ran never reads as green; shipped for
`verify_changes` as ANTS-1289 INV-14).

**Layman:** Claude's test tools can read and run the tests of Python,
JavaScript, Rust and Go projects, not only this project's own, and say
plainly when no test ran at all.

## 1. Problem

After this ships, `test_results op:"record"` accepts the output of pytest,
jest, vitest, `cargo test` and `go test -v` as well as ctest, and
`focused_test` runs the tests a change touches under any of those six
runners. Both replies name the runner and say `nothing_ran:true` when
recognised output shows no test ran.

What is missing today:

1. **`test_results` refuses every runner but ctest.**
   `RemoteControl::cmdTestResults` (`src/remotecontrol_coldeyes.cpp`) parses
   with `TestResCache::parseCtestOutput` only, and refuses anything else with
   `bad_args`, "does not look like ctest --output-on-failure output".
   ANTS-1300 § 6 put other parsers out of scope for v1.
2. **`focused_test` runs ctest only.** `RemoteControl::cmdFocusedTest`
   (`src/remotecontrol_state.cpp`) refuses `no_build_dir` without a
   `CMakeCache.txt`, runs `ctest`, and parses with `parseCtestOutput`
   (ANTS-1302 § 3.4). A Python, JavaScript, Rust or Go project gets nothing.
3. **A run that executed nothing reads like a clean run.** A recognised
   output with `total:0` is recorded and returned with `failed:0` and no
   other signal. `focused_test` re-runs the full suite when its selection
   matched nothing (ANTS-1302 INV-16), but a full run that also reports 0
   returns `ok:true`, `failed:0`. ANTS-5498 fixed the same hole in
   `verify_changes` only.
4. **The pieces that exist are partial and private.** `autoDetect`
   (`src/verifyengine.cpp`) sits in an anonymous namespace, has no jest,
   vitest or go case, and runs plain `npm test`. `MutationProbe::parseCounts`
   (`src/mutationprobe.h`) reads ctest, jest, vitest, pytest and gtest, but
   returns only `passed` and `failed`, with no skipped count, total or test
   names, and nothing for cargo or go.

## 2. Surface

### 2.1 A shared runner library

New `src/testrunners.{h,cpp}`, Qt6::Core only, in `ants_core_lib`:

```cpp
namespace TestRunners {

enum class Runner { Ctest, Pytest, Jest, Vitest, Cargo, Go };

QString name(Runner r);                          // "ctest", "pytest", ...
std::optional<Runner> fromName(const QString &s); // exact, lower-case

struct Detection {
    std::optional<Runner> runner;  // empty = none found
    QString evidence;              // root-relative marker, e.g. "go.mod"
};
// § 2.2. Reads marker files under rootCanonical; spawns nothing.
Detection detect(const QString &rootCanonical);

// § 2.3. The first runner whose output shape matches, else empty.
std::optional<Runner> sniff(const QString &output);

// § 2.3. Pure. Fills the same caps ParsedTests carries for ctest.
TestResCache::ParsedTests parse(Runner r, const QString &output);

}  // namespace TestRunners
```

`TestResCache::ParsedTests` (`src/testrescache.h`) gains two members:

```cpp
QString runner = QStringLiteral("ctest");
int     collected = -1;   // -1 = the runner printed no collected count
```

`parse(Runner::Ctest, …)` is `TestResCache::parseCtestOutput`, unchanged.

### 2.2 Detection

`detect` checks these in order and returns the first hit:

| Runner | Marker under the root |
|--------|-----------------------|
| ctest | `CMakeLists.txt` |
| cargo | `Cargo.toml` |
| go | `go.mod` |
| vitest | `package.json` naming `vitest` in `dependencies` or `devDependencies` |
| jest | `package.json` naming `jest` in either, or carrying a top-level `jest` key |
| pytest | `pytest.ini`, `conftest.py`, `[tool.pytest` in `pyproject.toml`, or `[tool:pytest]` in `setup.cfg` |

ctest comes first so that this project, and every CMake project
`focused_test` serves today, keeps its current behaviour, including the
`no_build_dir` refusal. vitest comes before jest because a vitest project
often keeps jest-style config. A caller whose project mixes runners passes
`runner`.

### 2.3 Parsing

`sniff` tries ctest first (`parseCtestOutput(...).recognised`), then these
line shapes in table order, each matched at a line start after any leading
whitespace:

| Runner | Recognised by | Counts from |
|--------|---------------|-------------|
| pytest | the closing summary line: comma-separated `<n> <word>` items, or `no tests ran`, then `in <t>s`, optionally wrapped in `=` rules (`-q` drops them) | that line's `passed`, `failed`, `error(s)`, `skipped`, `xfailed`, `xpassed`; other words, such as `deselected` and `warning(s)`, are not counted |
| jest | `Tests:` … `<n> total`, or `No tests found` | the `Tests:` line |
| vitest | `Tests` … `(<n>)` | the `Tests` line |
| cargo | `test result: (ok\|FAILED). <n> passed` | the sum over every `test result:` line |
| go | `--- (PASS\|FAIL\|SKIP): ` or `=== RUN ` | one count per un-indented `--- ` line |

Count mapping: pytest `error(s)` count as failed, `xfailed` as skipped and
`xpassed` as passed. cargo `ignored` counts as skipped. `total` is
`passed + failed + skipped` for every runner but ctest, which keeps its own
summary. Go counts top-level tests only; an indented `--- ` line is a
subtest, and a failing subtest already fails its parent.

Failing test names come from pytest's `FAILED <id>` and `ERROR <id>` lines,
jest's `● <suite> › <name>` headers, vitest's `FAIL <file> > <name>` lines,
cargo's `test <name> ... FAILED` lines and go's un-indented `--- FAIL: <name>`
lines. Each excerpt is that test's own block of output, under the caps
ANTS-1300 § 3.2 sets: last 20 lines, 1 000 characters a line, 4 000 in all,
8 000 for `full_excerpt`, 50 entries.

`duration_ms` comes from pytest's `in <t>s`, jest's `Time:`, vitest's
`Duration` line, the sum of cargo's `finished in <t>s`, and the sum of go's
package lines (`ok`/`FAIL <pkg> <t>s`). A caller-supplied `duration_ms`
still wins.

`recognised` is false when no recognising line matched. Go output without
`-v` (package lines only) is not recognised, nor is a go run that ran no
test.

### 2.4 Choosing the tests to run (`focused_test`)

The changed-file set works as ANTS-1302 § 3.4 step 4 says. For every
runner, a valid coverage map's `ignore` globs drop matching files into
`ignored_files` first (ANTS-1302 § 3.3); with no valid map, only the
entries INV-10 drops are ignored. Then, per runner:

| Runner | Narrowed run | Full run | `selection` |
|--------|--------------|----------|-------------|
| ctest | as today (ANTS-1302 § 3.4 steps 3, 6) | as today | `map` / `heuristic` / `full` |
| pytest | `python3 -m pytest -rfE <paths>` | `python3 -m pytest -rfE` | `heuristic` / `full` |
| jest | `npx --no-install jest --ci --findRelatedTests <paths>` | `npx --no-install jest --ci` | `related` / `full` |
| vitest | `npx --no-install vitest related --run <paths>` | `npx --no-install vitest run` | `related` / `full` |
| cargo | `cargo test --no-fail-fast -p <pkg> [-p <pkg> …]` | `cargo test --no-fail-fast` | `related` / `full` |
| go | `go test -v <./dir> [<./dir> …]` | `go test -v ./...` | `related` / `full` |

- **The coverage map steers ctest only.** Its entries are `ctest -R`
  fragments (ANTS-1302 § 3.2). jest, vitest, cargo and go have their own
  way to pick related tests, which knows more than a hand-kept map.
- **pytest's heuristic.** A changed `test_*.py` or `*_test.py` runs itself.
  A changed source `foo.py` runs every `test_foo.py` and `foo_test.py` in the
  tree. A changed `.py` file with no test found, or any changed file that
  is not `.py`, makes the run full, as an unmapped file does under
  ANTS-1302 § 3.3. The walk skips dot-directories,
  `node_modules`, `build` and any directory holding `pyvenv.cfg`, and stops
  at 20 000 files with a full run and a reason saying so.
- **cargo's package** is the `[package] name` in the nearest `Cargo.toml`
  above the changed file. A changed file under no package runs full.
- **go's directory** is the changed `.go` file's directory. Any other
  changed file, `go.mod` and `go.sum` included, runs full.
- **jest and vitest** receive the changed files themselves and do their own
  dependency walk.

The 0-match safety net (ANTS-1302 INV-16) applies to every runner: a narrowed
run reporting `total == 0`, recognised or not, is re-run full once, with
`downgraded_to_full`. Only the final output must be recognised.

### 2.5 Arguments, envelope and refusals

Both verbs gain an optional `runner` argument, one of the six names. An
unknown name refuses `bad_args`.

- **`test_results op:"record"`**: `runner` given → parse as that runner;
  absent → `sniff`. Unrecognised output still refuses `bad_args`, now
  naming the six formats. `runner` on `op:"read"` refuses `bad_args`, as
  `detail` does on `op:"record"`.
- **`focused_test`**: `runner` given → use it; absent → `detect`. Nothing
  detected refuses `no_runner`.

Envelope additions, on every `test_results` reply that carries counts and
on `focused_test`:

| Field | Meaning |
|-------|---------|
| `runner` | the runner's name; a cached record with none reads as `ctest` |
| `nothing_ran` | `true` exactly when `total == 0` |
| `collected` | present only when the runner printed it (pytest's `collected N items`); never computed |

`duration_ms` stays the run's total. `focused_test` also returns
`runner_evidence` (the marker, when detected) and `selector` (the arguments
that narrowed the run, `[]` when full). `ctest_filter` and `build_dir` stay,
present only when the runner is ctest.

New refusal codes, registered in `docs/standards/mcp-error-codes.md`:

| Code | When | Caller's next move |
|------|------|--------------------|
| `no_runner` | `focused_test` found no marker and got no `runner` | pass `runner` |
| `runner_missing` | a non-ctest runner's program failed to start | install the runner |
| `runner_failed` | a non-ctest run crashed or timed out | read the captured tail |

ctest keeps `ctest_missing` and `ctest_failed`, so current callers see no
change.

### 2.6 Running a project's tests

`focused_test` already runs code the project owns, through ctest, and
`verify_changes` does the same. This adds no new kind of trust. Every
runner starts through `HostExec::start` (`src/hostexec.h`), as ctest does.
Inside a Flatpak that starts `flatpak-spawn`, so a missing runner there
refuses `unrecognised_output`, not `runner_missing`. `focused_test` passes
arguments as a list, never through a shell. `npx --no-install` stops a run
from downloading a package. A changed-file path reaches the command line
only as a root-relative path beginning `./`, so an entry such as
`--config=x` can never be read as an option (INV-10).

### 2.7 Compatibility

`tests.json` keeps `schema_version: 1`. `runner` and `collected` are new
optional keys; a record without `runner` reads as ctest (INV-8). Every
current ctest caller sees the same fields and refusals plus the § 2.5
additions (INV-13).

## 3. Invariants

- **INV-1** — `detect` returns the first runner in § 2.2's order whose
  marker exists, with that marker as `evidence`, and empty when none does.
  *Test:* `tests/features/testrunners_detect/` builds one scratch root per
  row, plus one holding both `CMakeLists.txt` and `package.json` with
  `jest`, which must detect ctest.
  *Breaks when:* the order changes, or a `package.json` with no `jest` or
  `vitest` key detects jest.

- **INV-2** — `sniff` returns ctest for any output `parseCtestOutput`
  recognises, before trying another runner.
  *Test:* `tests/features/testrunners_parse/` sniffs copies of the ctest
  fixtures in `tests/features/testrescache_parse/`, plus a ctest log
  carrying a pytest summary line.
  *Breaks when:* a ctest log holding a pytest-shaped line is sniffed as
  pytest.

- **INV-3** — For each of the five new runners, `parse` of a captured real
  run yields the `passed`, `failed`, `skipped` and failing names that run
  printed, and `total` as § 2.3 computes it.
  *Test:* `tests/features/testrunners_parse/` — one fixture per runner with
  at least one pass, one failure and one skip, captured from a real run, the
  runner's version recorded beside it.
  *Breaks when:* a count is read from the wrong token, or a failure is
  missed.

- **INV-4** — pytest `error(s)` count as failed, `xfailed` as skipped,
  `xpassed` as passed; cargo `ignored` as skipped; cargo's counts are
  summed over every `test result:` line.
  *Test:* `tests/features/testrunners_parse/` — a pytest fixture with an
  error and an xfail, and a cargo fixture with two test binaries.
  *Breaks when:* only the first `test result:` line is read, or errors are
  dropped.

- **INV-5** — Go counts un-indented `--- ` lines only, and go output with
  no `--- ` or `=== RUN ` line is not recognised.
  *Test:* `tests/features/testrunners_parse/` — a go fixture with a failing
  subtest under a failing parent counts one failure; package-lines-only
  output gives `recognised:false`.
  *Breaks when:* subtests are counted, or non-`-v` output is accepted with
  zero counts.

- **INV-6** — `nothing_ran` is `true` exactly when `total == 0`, on every
  runner, on `test_results` replies and on `focused_test`.
  *Test:* `tests/features/mcp_test_results/` records pytest's `no tests ran`
  output and reads `nothing_ran:true`; it records a passing ctest log and
  reads `nothing_ran:false`.
  *Breaks when:* the field is computed from `failed`, or omitted on ctest.

- **INV-7** — `collected` appears only when the runner printed a collected
  count, and is never derived from other counts.
  *Test:* `tests/features/testrunners_parse/` — a pytest fixture with
  `collected 7 items / 2 deselected` gives `collected:7`; the jest fixture
  gives no `collected` key on the wire.
  *Breaks when:* `collected` is filled from `total`.

- **INV-8** — `test_results op:"record"` with no `runner` parses as the
  sniffed runner and stores it; with `runner` it parses as that runner; an
  unknown `runner` refuses `bad_args`. A cache record with no `runner` key
  reads back as `ctest`.
  *Test:* `tests/features/mcp_test_results/`.
  *Breaks when:* a cargo log is stored with `runner:"ctest"`, or an old
  record fails to load.

- **INV-9** — `focused_test` builds the command § 2.4's table names for each
  runner and selection.
  *Test:* `tests/features/mcp_focused_test/` checks the argument list a pure
  builder returns, without running anything.
  *Breaks when:* jest loses `--no-install` or `--ci`, cargo loses
  `--no-fail-fast`, or go loses `-v`.

- **INV-10** — For every runner but ctest, no changed-file entry reaches
  the argument list except as a root-relative path beginning `./`, and an
  entry outside the root is dropped and listed in `ignored_files`, so it
  does not change the selection. ctest resolves entries as ANTS-1302 § 3.3
  says.
  *Test:* `tests/features/mcp_focused_test/` passes
  `changed_files:["--config=x.js", "../outside.py"]` to the builder for
  jest and pytest.
  *Breaks when:* `--config=x.js` appears in the list as given.

- **INV-11** — The 0-match net re-runs full for every runner, as ANTS-1302
  INV-16 does for ctest.
  *Test:* `tests/features/mcp_focused_test/` with a stub runner whose
  narrowed output is jest's `No tests found`.
  *Breaks when:* the net checks `runner == ctest`.

- **INV-12** — `focused_test` refuses `no_runner` when `detect` finds
  nothing and no `runner` was passed; a non-ctest program that fails to
  start refuses `runner_missing`; a non-ctest crash or timeout refuses
  `runner_failed`. ctest keeps `no_build_dir`, `ctest_missing` and
  `ctest_failed`.
  *Test:* `tests/features/mcp_focused_test/` — an empty scratch root, and
  `runner:"go"` run with an empty search path.
  *Breaks when:* a missing `go` reads as `ctest_missing`.

- **INV-13** — On a ctest project the `focused_test` envelope keeps every
  field ANTS-1302 INV-17 lists, and adds only `runner`, `runner_evidence`,
  `selector`, `nothing_ran` and `collected`.
  *Test:* `tests/features/mcp_focused_test/` compares the envelope's key set
  on a ctest scratch project, called with no `runner`, with the fields
  ANTS-1302 § 3.5 lists plus `runner`, `runner_evidence`, `selector` and
  `nothing_ran`. ctest prints no collected count, so `collected` is absent
  (INV-7).
  *Breaks when:* `ctest_filter` or `build_dir` disappears for ctest.

## 4. Failure modes

- **A runner changes its output format.** `parse` fails to recognise it and
  the verb refuses rather than reporting zeros. The fixtures pin the
  versions tested.
- **A project mixes runners.** `detect` picks one by § 2.2's order. The
  reply names it in `runner` and `runner_evidence`, so the caller can see
  the choice and pass `runner`.
- **`npx --no-install` cannot find jest or vitest.** npx exits non-zero with
  its own message. The output is unrecognised, so the verb refuses
  `unrecognised_output` with the captured tail, which names the problem.
- **jest's or vitest's related-test walk finds nothing.** The 0-match net
  runs the full suite.
- **A pytest tree too big to walk.** The walk stops at 20 000 files and the
  run goes full, with the reason in `selection_reason`.
- **A build step hides inside the test command.** `cargo test` and
  `go test` compile first, so a narrowed run can still take minutes. The
  existing `timeout_sec` bound (ANTS-1302 § 3.4 step 7) applies.

## 5. RAM / build cost

No new state and no new cache. The pytest walk is capped at 20 000 files.
Every other cap is ANTS-1300's and ANTS-1302's, unchanged. No new library
dependency; the new source files join `ants_core_lib`.

## 6. Out of scope

- `verify_changes` detecting runners through `TestRunners::detect` —
  deferred; not yet queued.
- `MutationProbe::parseCounts` reading cargo and go through
  `TestRunners::parse` — deferred; not yet queued.
- Per-test durations — not done, as for ctest today.
- Other runners: `unittest`, mocha, `cargo nextest`, `go test -json`.
- Recording `focused_test` runs into the `test_results` cache — not done,
  because ANTS-1302 § 6 keeps the two apart.

## 7. Tests

- `tests/features/testrunners_detect/` — INV-1.
- `tests/features/testrunners_parse/` — INV-2, INV-3, INV-4, INV-5, INV-7.
  Fixtures are captured from real runs; this host has pytest and cargo,
  while go, jest and vitest must be installed before their fixtures can be
  captured (§ 12).
- `tests/features/mcp_test_results/` (extended) — INV-6, INV-8.
- `tests/features/mcp_focused_test/` (extended) — INV-9, INV-10, INV-11,
  INV-12, INV-13. The command builder is pure, so INV-9 and INV-10 run no
  runner.

Label `features;fast`. Each new assertion is seen failing against the code
before this change.

## 8. Reload

Both verbs are registered in `mcp::registerProjectScopedVerbs()`
(`src/mcptoolregistry.cpp`), so `ants-mcpd` serves them. A rebuilt
`ants-mcpd` reaches clients on their next reconnect, with no terminal
relaunch. The library holds no running state, so it needs no stop/start
control. It runs inside `ants-mcpd`, as the verbs do today, because it is
part of those verbs.

## 9. Decisions and alternatives

None was put to the user. These calls are the author's (2026-10-21):

- **Six runners: ctest, pytest, jest, vitest, cargo, go** — the roadmap
  item's list, plus ctest.
- **Go output must be `go test -v`.** Without `-v`, go prints one line per
  package and no per-test result, so passed and skipped cannot be counted.
- **"Durations" in the roadmap item means the run's total `duration_ms`.**

Rejected:

- **Make `autoDetect` in `src/verifyengine.cpp` public and reuse it.** It
  has no jest, vitest or go case, and checks `package.json` before
  `Cargo.toml`. Changing it changes `verify_changes`, whose accepted spec
  (ANTS-1289 § 2.5) fixes that order. A new library leaves that contract
  alone.
- **Machine reporters** (`pytest --junitxml`, `jest --json`,
  `go test -json`). Sturdier to parse, but `test_results` records output
  the caller already produced, which is usually text. One text parser per
  runner serves both verbs.
- **Require `runner` on every call.** Detection is cheap and
  `runner_evidence` shows the choice, so a caller passes `runner` only when
  the guess is wrong.
- **Rename `ctest_missing` and `ctest_failed` to the runner-neutral codes.**
  That breaks every caller matching them today.
- **One coverage map for every runner.** jest, vitest, cargo and go already
  know which tests depend on a file; a hand-kept map would know less.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `tests/features/testrunners_detect/` |
| INV-2 | `tests/features/testrunners_parse/` |
| INV-3 | **`Partial:`** `tests/features/testrunners_parse/` for the pinned versions; a newer runner's format is caught only when a fixture is re-captured |
| INV-4 | `tests/features/testrunners_parse/` |
| INV-5 | `tests/features/testrunners_parse/` |
| INV-6 | `tests/features/mcp_test_results/` |
| INV-7 | `tests/features/testrunners_parse/` |
| INV-8 | `tests/features/mcp_test_results/` |
| INV-9 | `tests/features/mcp_focused_test/` |
| INV-10 | `tests/features/mcp_focused_test/` |
| INV-11 | `tests/features/mcp_focused_test/` |
| INV-12 | `tests/features/mcp_focused_test/` |
| INV-13 | `tests/features/mcp_focused_test/` |
| § 8 Reload | **nothing** — no test checks which registry function a verb is in |

## 11. Cross-doc impact

- `docs/specs/ANTS-1300.md` and `docs/specs/ANTS-1302.md` — a pointer line
  to this spec.
- `docs/standards/mcp-error-codes.md` — rows for `no_runner`,
  `runner_missing` and `runner_failed`.
- Both verbs' tool descriptions and the new `runner` argument, under the
  140-character cap `tests/features/mcp_eager_load/` checks.
- `docs/subsystems.md` — `src/testrunners.{h,cpp}`.
- `CHANGELOG.md` — one entry under `[Unreleased]`.

## 12. Open questions

- **Go, jest and vitest are not on this host.** Their fixtures need them
  installed: go through the system package manager, which needs the user;
  jest and vitest from npm, into a scratch directory on `/mnt/Games`.

## Cold-eyes loop log

Rows live in `../reviews/ANTS-5503-multi-runner-tests-loop-log.md`.
