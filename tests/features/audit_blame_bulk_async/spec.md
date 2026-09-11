# Feature: audit blame enrichment is one `git blame` per file, off the GUI thread (ANTS-5040)

## Contract

After an audit run (or a cancel), `AuditDialog::renderResults()` enriches up
to `kSnippetBudget` (300) findings with git-blame metadata (author, date,
short sha) so the results list can show "who touched this line, and when".

Today that enrichment runs one `git blame -L N,N HEAD` `QProcess` per
finding, synchronously, on the GUI thread — up to 300 processes, one after
another, each waiting up to 2 s (`enrichWithBlame`, called from
`renderResults`'s enrichment loop). `git blame` on an old line walks the
file's history rather than reading a fixed offset, so on a large repository
the render this triggers is tens of seconds to minutes of a frozen dialog,
not the "bounded (cheap but bounded)" the code comment beside
`kSnippetBudget` claims — that comment is true only in the worst-case-count
sense, not in the wall-clock sense a user experiences it in.

The fix has two independent parts, and this spec locks both as separate
invariants because either one can regress without the other:

1. **Batch the git calls.** One `git blame --line-porcelain` invocation per
   *file*, carrying every needed line as a `-L a,a` pair, rather than one
   invocation per *finding*. `--line-porcelain` (not `--porcelain`) is
   required because a plain `--porcelain` invocation only repeats the full
   commit header the first time a commit is seen in the output — with
   several `-L` ranges landing on different commits, only the first would
   carry `author`/`author-time` at all. `src/gitblameparse.h`
   (`GitBlame::parseLinePorcelain`) is the parser for that batched output,
   already wired in as a header-only seam and stubbed to return nothing.
2. **Move the process off the GUI thread.** No code on the path
   `renderResults` reaches for blame data may call `QProcess::waitForFinished`
   synchronously; the result must be applied to the cache asynchronously
   (however it is delivered — a signal, a callback, a future) and the
   dialog re-rendered when it lands.

## Rationale

Found by two lanes (audit, threading). Not measured end-to-end (no repo
large enough was profiled here), but the mechanism is not in question:
`git blame` on an old line is a history walk, `kSnippetBudget` is 300, and
every one of those 300 calls currently blocks the thread that also has to
keep painting the dialog.

## Invariants

- **INV-1** — `GitBlame::parseLinePorcelain` correctly maps every requested
  final line to its author, short sha and commit date from one
  `--line-porcelain` invocation covering several `-L` ranges, including
  when the ranges land on different commits. `--line-porcelain` repeats
  the full commit block (author / author-time / author-tz / ...) for every
  line rather than only the first occurrence of a commit, which is the
  entire reason the fix asks for that flag instead of plain `--porcelain`;
  the parser must consume that repetition rather than assume one header per
  commit. Date is the `author-time` epoch converted the same way the
  existing single-line parser does it
  (`QDateTime::fromSecsSinceEpoch(t).toString("yyyy-MM-dd")` — local time,
  not UTC); sha is the first eight hex characters of the blamed commit.
  *Test:* `test_audit_blame_bulk_async.cpp`,
  `LinePorcelainParserCoversAllRequestedLinesAcrossCommits` — a real `git
  init` fixture with two commits (so two different shas appear in one
  `--line-porcelain` output) and a real `git blame --line-porcelain -L a,a
  -L b,b -L c,c` invocation; skips (`GTEST_SKIP`) only when `git` is not on
  `PATH`.

- **INV-2** — the blame path reachable from `renderResults()` never blocks
  the GUI thread on `git blame`, and the process it starts is batched per
  file rather than per finding. Neither `AuditDialog::enrichWithBlame`
  (while it still exists — the intended fix may fold it into an async
  helper and this invariant tolerates that) nor `AuditDialog::renderResults`
  itself calls `QProcess::waitForFinished` on the blame path. The `git
  blame` argv built anywhere in the file uses `--line-porcelain` (not the
  single-line `--porcelain` form) and constructs more than one `-L` range
  per process invocation — the loop-based construction the fix note
  describes, not one process per finding. *Test:*
  `test_audit_blame_bulk_async.cpp` (source-scrape, comments stripped —
  `AuditDialog` is a `QDialog`; this project's house pattern for it is
  source-scrape, not construction — see `audit_dialog_render_hardening`,
  `audit_tool_process_group_kill`), three TESTs:
  `NoSynchronousWaitInTheBlamePath`, `ArgvUsesLinePorcelainFlag`,
  `ArgvBuildsMultipleRangesPerProcess`. The needles were chosen to admit any
  correct async, batched implementation rather than pin one spelling of it:
  the wait-check reads whichever of the two named functions still exists
  rather than requiring a specific one to survive the refactor; the
  multi-range check accepts either an unrolled argv (`"-L"` written out
  more than once) or a single `"-L"` literal built inside a visible loop
  construct (`for`/`while`), because a loop-based implementation only needs
  the literal once in source even though it runs once per line at runtime.

## Scope

### In scope
- `GitBlame::parseLinePorcelain` (`src/gitblameparse.h`) — the batched
  `--line-porcelain` parser.
- The blame call site(s) reachable from `AuditDialog::renderResults()` —
  today `enrichWithBlame`, or whatever it is refactored into.

### Out of scope
- The re-render-when-it-lands wiring itself (which signal/callback shape
  delivers the async result). INV-2 asserts only that the *current* call
  does not block; it does not pin the delivery mechanism.
- `AuditDialog`'s behaviour end to end (a `QDialog`; house pattern is
  source-scrape, not construction).
- Any UI change to *when* blame is fetched (e.g. "blame only when a finding
  is expanded" — the roadmap item's alternative fix). Either fix satisfies
  INV-2 as written, since both get the process off the GUI thread; only
  the batched, one-process-per-file shape is asserted, not the trigger.
- `m_blameCache`'s key shape and the traversal guard in `enrichWithBlame`
  (`QDir::isAbsolutePath`, `..` rejection) — unrelated to this defect and
  already covered by ANTS-2003, unchanged by this fix.

## Regression history

- **ANTS-5040 (found by two lanes: audit, threading):**
  `AuditDialog::renderResults()`'s enrichment loop calls
  `enrichWithBlame(f)` synchronously for up to `kSnippetBudget` (300)
  findings; `enrichWithBlame` runs `git blame --porcelain -L N,N HEAD --
  <file>` and calls `QProcess::waitForFinished(2000)` on the GUI thread, one
  process per finding. `src/gitblameparse.h` ships as a stub
  (`parseLinePorcelain` returns an empty map unconditionally) pending the
  fix. This spec locks the contract before the fix lands; both invariants
  are expected to fail against the current tree — INV-1 because the parser
  is a stub, INV-2 because `enrichWithBlame` still blocks the GUI thread and
  the file has no `--line-porcelain` invocation at all.
