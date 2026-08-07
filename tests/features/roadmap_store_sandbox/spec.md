# roadmap_store_sandbox — the test harness cannot reach the user's data directory

**ANTS-3856.** `RoadmapStore::defaultPath()` resolves under
`QStandardPaths::GenericDataLocation` — the user's live roadmap database — and
the constructor's `dbPath` defaults to it. So `RoadmapStore store;` in a test
opens the developer's real data and succeeds. That is not hypothetical: on
2026-08-06 the live store on this machine held exactly one `project` row, root
`/tmp/test_core-ZnzBrv`, name `Demo`, slug `demo`, 0 sections and 0 items. A
fixture, in the user's data directory.

It was harmless only because nothing had ever migrated a real project into that
store. ANTS-3855 landed the `roadmap_migrate` verb, so a production trigger now
exists: the same slip no longer leaks an empty row, it overwrites a migrated
project.

The remedy is not a convention. "Always pass an explicit path" is what already
failed, and it fails silently — a test that omits the path passes.

## The mechanism, and the two it replaced

Each bundle `main()` points `XDG_DATA_HOME` at a per-process `QTemporaryDir`
before any test body runs, and exits 1 if that directory cannot be created.
`GenericDataLocation` follows it, so `defaultPath()` resolves inside the sandbox
for every reader. **No production code changes.**

Per-process rather than a fixed directory because ctest runs the bundles at
`-j4`; a shared one would let two processes see each other's files.

Two other designs were implemented and measured first, and both are worth
recording because each looked correct until the suite ran:

1. **Refusal** — `RoadmapStore::open()` rejecting `defaultPath()` under the
   harness. It failed 14 tests: `roadmap_log_prefix` (5),
   `roadmap_log_possible_duplicates` (5), `changelog_log_writer` (3),
   `changelog_log_add_batch` (1). Those drive the `roadmap_log` and
   `changelog_log` verbs, whose handlers resolve `defaultPath()` *inside
   themselves* — the test has no path to pass and nothing its author could fix
   short of a per-verb seam. They had been reaching the live store on every run,
   and refusing converted that into `read_failed` rather than into a fix.
2. **Store-side substitution** — the constructor building at a sandbox path when
   asked for `defaultPath()`. It failed the 9 `RoadmapWriteHalf` tests, and the
   reason generalises: `RoadmapSource::storeFor()` **stats** `defaultPath()` and
   returns `nullptr` when the file is absent, without ever constructing a store.
   A redirect applied store-side leaves the stat looking at one file and the
   open at another, so a project migrated into the sandbox is invisible to the
   consumer that asked for it. **A path redirect has to happen where the path is
   resolved, not where it is opened.**

`XDG_DATA_HOME` is also the mechanism the suite already trusts per-test
(`ants_test::XdgGuard`, ANTS-2062), and `roadmap_write_half` was already using
it correctly — this generalises that file's local fix to every bundle. It covers
every writer under `GenericDataLocation`, not only the store. A test that needs
its own sandbox still overrides it, and `XdgGuard` restores it to this one.

Qt's own `QStandardPaths::setTestModeEnabled(true)` was not used. This project
already ruled it out for this class of sandboxing — "it routes to a
Qt-test-specific path which has collided with real user configs in practice"
(`tests/features/config_parse_failure_guard`) — and tests here toggle it in both
directions (ANTS-2062, ANTS-2151), so a process-wide flip in `main()` would not
survive the suite.

## Invariants

- **INV-1** — A test process is armed: `XDG_DATA_HOME` is set from `main()`
  before any test body, names a directory that exists, and is not the user's
  real data directory.
  *Test:* `qEnvironmentVariable("XDG_DATA_HOME")` non-empty, `QDir::exists()`,
  and `!=` `$HOME/.local/share` — derived from `HOME`, not from
  `QStandardPaths`, which is the thing under test and would agree with itself.
- **INV-2** — `RoadmapStore::defaultPath()` resolves inside the sandbox. This
  is the invariant, not "a constructed store is sandboxed": the readers that
  matter most never construct one — `RoadmapSource::storeFor()` stats the path —
  and that is what design 2 above got wrong.
  *Test:* `defaultPath() == $XDG_DATA_HOME + "/ants-terminal/roadmap.sqlite"`,
  and it does not start with the real data home.
- **INV-3** — A default-constructed `RoadmapStore` — the exact shape that
  leaked — opens inside the sandbox, and is a real store: the leak was one
  `registerProject()` away, so the test takes that step too.
  *Test:* `RoadmapStore store;` → `path()` under `$XDG_DATA_HOME`, `open()`
  succeeds, the file exists, `registerProject()` succeeds.
  **The write is gated on an `ASSERT` against the HOME-derived path, not on
  the `$XDG_DATA_HOME` comparison.** Measured 2026-08-07: comparing only
  against the variable the sandbox sets makes the check vacuous exactly when
  the arming is missing but the variable is inherited from the developer's
  shell — and this machine exports `XDG_DATA_HOME=$HOME/.local/share`, so
  disarming `main()` made this very test register a fixture project in the
  live store. A test whose failure mode is the bug it guards is worse than no
  test.
- **INV-4** — A store given a path of its own is untouched. Every other roadmap
  test in the suite is this case.
  *Test:* `QTemporaryDir` path in, same path out, opens.
- **INV-5** — Every bundle main arms it, and per-process.
  `tests/bundle_main_core.cpp` and `tests/bundle_main_gui.cpp` between them are
  `main()` for every GoogleTest bundle; this test compiles into `test_core`
  only, so INV-1 can prove nothing about the GUI bundles. A source scrape is the
  only thing that reaches them.
  *Test:* every `tests/bundle_main_*.cpp` contains `qputenv("XDG_DATA_HOME"`
  **and** `QTemporaryDir` — the second because a fixed path would pass the first
  while reintroducing the parallel-run collision.

## Not covered

- **A path-omitting test still passes**, against a throwaway data directory that
  dies with the process. That is the price of unblocking the 14 verb-driven
  tests above. Two tests in one *process* would share it; ctest runs one test
  per process, so today the blast radius is a single test.
- **Only `XDG_DATA_HOME`.** `XDG_CONFIG_HOME` and `XDG_CACHE_HOME` are left to
  the per-test guards that already handle them — this item is about the data
  directory, and widening it would be a change no failure has asked for.
- INV-5 is a rename guard, not a disable guard: a bundle main that keeps the
  `qputenv` call but neuters it still scrapes clean. INV-1 is what catches that,
  in whichever bundle this test runs in.
