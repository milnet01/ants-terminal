# roadmap_last_touch_async — the last-touch blame stops holding the GUI thread

ANTS-4414. Contract for `RoadmapDialog`'s in-progress "last touched" dates
being fetched without blocking the dialog's open.

## What was wrong

`refreshLastTouchDatesIfStale()` called `parseLastTouchDates()`, which ran
`git blame --line-porcelain` over the whole of `ROADMAP.md` and waited for it,
on the GUI thread, from `rebuild()`.

Measured 2026-08-17 on the Ants roadmap (45,832 lines / 3.6 MB):

| Stage | Time |
|---|---|
| `git blame --line-porcelain` (whole file) | **3.71 s** |
| `loadMarkdown` | 3 ms |
| `renderCardsHtml` | 57 ms |
| `extractToc` | 4 ms |
| `setHtml` + layout | 11 ms |

So the blame was 3.71 s of a 3.79 s open — 98% — and it ran on **every** open,
because `mainwindow.cpp` constructs the dialog with `WA_DeleteOnClose` and the
in-memory mtime guard is therefore born stale each time.

Two measurements that rule out the obvious alternatives, recorded so they are
not re-attempted:

- **Restricting the blame to the lines actually needed does not help.**
  `git blame -L` over just the four 🚧 blocks is 3.12 s against 3.71 s — 16%.
  The cost is history traversal, not line count.
- **A persistent cache is not the fix.** Blame reflects working-tree state, and
  every `roadmap_log` write re-renders `ROADMAP.md`, so any mtime-keyed cache
  is invalidated on essentially every open.

The payload is four cards: the result is consumed only for `- 🚧 [ID]` bullets,
of which this roadmap has 4 out of 2,031.

## Invariants under test

- **INV-1.** `refreshLastTouchDatesIfStale()` returns before the blame has
  produced an answer. Asserted structurally, not by a stopwatch: immediately
  after the call the dates hash is still empty, and it becomes populated only
  after the event loop runs. A timing bound would be flaky; this cannot pass
  against a synchronous implementation.

- **INV-2.** The dates do arrive. After the event loop spins, a 🚧 bullet in a
  committed fixture repo has an author-time.

- **INV-3.** The result reaches the screen. Landing dates triggers a re-render
  rather than sitting in a hash nobody reads.

- **INV-4.** `lastTouchFromBlame()` is a pure function of (blame output,
  roadmap file) and takes MAX over the bullet's block — the bullet line plus
  every contiguous 2-space-indented continuation, stopping at a blank line, a
  new bullet, or EOF. This is the half that was previously unreachable without
  spawning git; splitting it out is what makes the rule testable.

- **INV-5.** Only one blame runs at a time. `rebuild()` fires per filter toggle
  and per debounced keystroke, so a second call while one is in flight must not
  start another process.

- **INV-6.** A completed run is not repeated for the same mtime **even when it
  produced nothing**. "No 🚧 bullets" and "not a git repo" both yield an empty
  hash, so a guard keyed on the hash being non-empty would re-run forever. The
  guard is a separate "ran" flag; this invariant is what it exists for.

- **INV-7.** `parseLastTouchDates()` — the synchronous form — still works and
  still honours its 5 s budget. ANTS-1237's tests drive it directly, and a
  caller that genuinely needs the answer before returning still has one.

## Test shape

gtest in the `test_dialogs` bundle. INV-2, INV-3 and INV-5 need a real git
repository, so the fixture runs `git init` + one commit in a `QTemporaryDir`
with `user.email` / `user.name` set locally (the machine's global identity must
not be a prerequisite, and `commit.gpgsign` is disabled for the same reason).
Tests requiring git skip with a clear message when git is absent rather than
failing — the same posture the production path takes.

INV-4 needs no git at all: it feeds synthetic `--line-porcelain` text straight
to the parser.
