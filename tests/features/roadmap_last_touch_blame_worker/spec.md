# roadmap_last_touch_blame_worker — blame's own text decides the dates, and parsing leaves the GUI thread

ANTS-5047. Contract for the second half of `RoadmapDialog`'s in-progress
"last touched" dates: `lastTouchFromBlame()`'s parse still ran on the GUI
thread and still re-read `ROADMAP.md` from disk after ANTS-4414 moved the
`git blame` *process* to the background.

## Regression history

- **ANTS-1237** added the synchronous `parseLastTouchDates()` — `git blame
  --line-porcelain` over the whole file, waited for, on the GUI thread.
- **ANTS-4414** split it into `lastTouchBlameArgs()` +
  `lastTouchFromBlame()` and moved the `QProcess` off the GUI thread's
  wait, via `refreshLastTouchDatesIfStale()`'s `finished` signal. That
  fixed the *waiting*; it did not touch what the `finished` handler does
  once the process exits.
- **ANTS-5047** (this contract) is what the `finished` handler still does,
  synchronously, on the GUI thread, every time it fires: split the whole
  `--line-porcelain` output (tens of MB at today's `ROADMAP.md` size,
  against the ~4 MB transient `docs/specs/ANTS-1237.md` § 6 budgets),
  re-read the whole file with no size cap, and run a decode + a regex over
  every line. It also races — `lastTouchFromBlame()` re-opens
  `m_roadmapPath` *after* blame has already finished, so a `roadmap_log`
  write landing in between attributes blame's dates to whatever bullets
  happen to sit at those line numbers in the *new* file content, not the
  content blame actually saw.

## What is broken

- `void RoadmapDialog::refreshLastTouchDatesIfStale()` (`src/roadmapdialog.cpp`)
  calls `lastTouchFromBlame(git->readAllStandardOutput(), m_roadmapPath)`
  directly inside the `QProcess::finished` lambda — no worker thread.
- `QHash<QString, qint64> RoadmapDialog::lastTouchFromBlame(const
  QByteArray &blameOut, const QString &roadmapPath)` uses `blameOut` only
  to build a per-line author-time array, then **re-opens `roadmapPath`
  from disk** and walks *its* lines to find `- 🚧 [ID]` bullets. The dates
  it hands out belong to whatever the file says NOW, not to what blame
  described.
- `QStringList RoadmapDialog::lastTouchBlameArgs(const QString &fileName)`
  passes `--line-porcelain`, which repeats the full commit header on every
  line — the whole-file cost this contract exists to cut.

## Invariants

- **INV-1.** `lastTouchFromBlame()` attributes a `- 🚧 [ID]` bullet's date
  from the bullet **text blame itself reports**, not from a re-read of
  `roadmapPath`. A blame whose content lines carry a bullet that the file
  on disk no longer has at that line (or has under a different id) still
  produces the date for the id blame's text names — this is the
  write-race ANTS-5047 describes, caught without needing an actual
  concurrent write.

- **INV-2.** Feeding `--porcelain`-shaped input (full header only on a
  commit's first appearance; a later, non-contiguous group from the same
  commit carries just `<sha> <orig> <final>` and no `author-time` line)
  still attributes that later group's lines to **that commit's own**
  author-time, not to whatever commit's header most recently printed one.
  A parser holding one running "current author-time" variable instead of
  a per-sha lookup gets this wrong the moment an intervening commit's
  header appears between two groups of an earlier commit.

- **INV-3.** `lastTouchBlameArgs()` passes `--porcelain`, not
  `--line-porcelain`. `--porcelain` prints the header block once per
  commit rather than once per line, which is what makes INV-2's per-sha
  lookup necessary — and is what cuts the transient output size this
  contract exists for.

- **INV-4.** `refreshLastTouchDatesIfStale()`'s `finished` handler never
  calls `lastTouchFromBlame()` (or does any comparable line-splitting,
  decoding or regex work) directly on the GUI thread. The parse runs
  inside a `QThread::create(...)` worker.

- **INV-5** (guard, not a red invariant — this must hold both before and
  after the fix). The block rule `lastTouchFromBlame()` applies — MAX
  author-time over a `- 🚧 [ID]` bullet's block: the bullet line plus every
  contiguous 2-space-indented continuation line, stopping at a blank line,
  a new bullet, or EOF — is unchanged by *where* the line text comes from.
  Tested with blame content and file content agreeing, so it passes
  identically whichever source today's implementation or the fixed one
  reads from.

## Scope

In scope: `RoadmapDialog::lastTouchFromBlame()`,
`RoadmapDialog::lastTouchBlameArgs()`, and the `finished`-handler wiring in
`RoadmapDialog::refreshLastTouchDatesIfStale()`. Out of scope: the
in-flight/one-at-a-time guard, the mtime staleness check, and the
synchronous `parseLastTouchDates()` form — all already contracted by
`tests/features/roadmap_last_touch_async/spec.md` (INV-1, INV-5, INV-6,
INV-7) and by `tests/features/roadmap_inprogress_age/spec.md`; this
contract must not require changing either. A new sibling directory rather
than an addition to `roadmap_last_touch_async/spec.md` was the caller's
call: that document's invariants describe the async *dispatch* (still
correct and unchanged), where this one describes what happens to the
*bytes* once the process exits — a different failure class from the same
subsystem.

## Test shape

gtest in the `test_dialogs` bundle (`CMakeLists.txt` — same bundle as
`roadmap_last_touch_async`, sharing its `ROADMAPDIALOG_CPP` /
`ROADMAPDIALOG_H` compile definitions). INV-1, INV-2 and INV-5 call
`RoadmapDialog::lastTouchFromBlame()` directly with synthetic blame text —
no git process, no dialog construction. INV-3 and INV-4 are source scrapes
over `src/roadmapdialog.cpp` (comments stripped via
`ants_test::stripComments`), anchored on the qualified
`RoadmapDialog::lastTouchBlameArgs(` and
`void RoadmapDialog::refreshLastTouchDatesIfStale(` signatures via
`ants_test::slurpFunctionBody()`.

INV-1 through INV-4 are expected to **fail** against the code as of
ANTS-5047's filing — that is the point of this contract. INV-5 is expected
to pass unchanged; it is a regression guard for the fix, not a lock on the
defect.
