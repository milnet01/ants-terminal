# ANTS-5078 — Stream scrollback and block exports to the file in slices

**Status:** accepted (2026-09-17), review-contract loop 1 at the user's one-pass cap (one lane, user decision).
**Kind:** review-fix.
**Source:** ROADMAP.md ANTS-5078, medium "exports on the GUI thread" (code-quality-review-2026-09-11 perf pass, lane terminal-widget-b).
**Composes with:** ANTS-5079 (`MainWindow` export through `QSaveFile`); `tests/features/terminalwidget_export_safety` (export write checks).

## 1. Problem

1. `TerminalWidget::exportAsText` and `TerminalWidget::exportAsHtml` build
   the whole export as one `QString` on the GUI thread. Their callers then
   convert it with `toUtf8()`. A large scrollback holds several full copies
   and freezes every tab until the write ends.
2. `TerminalWidget::exportBlockAsCast` builds `outputTextAt`, an escaped
   copy of it, the event string and its UTF-8 bytes, all at once.
3. Callers: the "Export Scrollback as Text..." and "Export Scrollback as
   HTML..." actions and "Share Block as .cast..." in
   `TerminalWidget::contextMenuEvent`, and the export action in
   `MainWindow::setupSettingsMenu`.

## 2. Surface

### 2.1 Decision: slices on the GUI thread, not a worker

The export runs on the GUI thread in slices. Each slice formats a bounded
number of lines and writes them straight to a `QSaveFile`. Between slices
the event loop runs.

Rejected: a worker thread.

- `TerminalWidget::onVtBatch` calls `TerminalGrid::processAction` on the
  GUI thread, so a worker cannot read the grid while output arrives.
- A worker would need a snapshot. `TerminalGrid` keeps scrollback as
  `std::deque<TermLine>` of `Cell` vectors, so a snapshot copies more
  memory than the text it exports.

Accepted cost: output arriving during an export can make the export fail
(§ 2.3). The user can retry.

### 2.2 `ScrollbackExporter` — `src/scrollbackexporter.h`, `src/scrollbackexporter.cpp`

A plain class, no `QObject`, in `ants_vt_lib` beside `src/terminalgrid.cpp`.
It reads only `TerminalGrid`, so a test drives it without a widget.

```cpp
class ScrollbackExporter {
public:
    enum class Format { Text, Html, Cast };
    enum class Step { More, Done, Failed };

    struct Request {
        Format format = Format::Text;
        QString path;
        QColor defaultFg, defaultBg;   // Html
        int fontPointSize = 0;         // Html
        quint64 blockId = 0;           // Cast: PromptRegion::id
        QString command;               // Cast: the block's command text
    };

    ScrollbackExporter(const TerminalGrid &grid, Request request);

    bool open();                  // opens the QSaveFile, writes the header
    Step step(int maxLines);      // formats and writes up to maxLines lines
    QString error() const;        // set when open() fails or step() is Failed
    qint64 bytesWritten() const;  // bytes handed to the file so far
};
```

- `open()` fixes the line range, the grid's `cols()` and `rows()`, and
  `scrollbackPushed() - scrollbackSize()`. Text and Html cover every
  scrollback and screen line at that moment. Cast covers the output lines
  of the region `blockId` names, as `outputTextAt` computes them. A missing
  region fails `open()`.
- The screen rows in the range are copied from the live grid at `open()`,
  with their combining characters. There are at most `rows()` of them. Scrollback lines
  are read from the grid when their slice runs.
- A slice finds a scrollback line's current index by subtracting the
  offset recorded at `open()` from its current offset,
  `scrollbackPushed() - scrollbackSize()`.
- `step()` returns `Done` only after `QSaveFile::commit()` succeeds.
- The bytes written match the pre-change functions exactly: `exportAsText`,
  `exportAsHtml`, and `exportBlockAsCast`'s events. `open()` writes the HTML
  head and the cast header. The last `step()` writes the closing tags.
- A cast escapes each line as it writes it. The command event is written at
  `open()`. The output event is skipped when the range is empty, as today.
- When the user has scrolled up, today's export reads the frozen screen
  snapshot (`TerminalWidget::cellAtGlobal`). That repeats lines pushed
  since the freeze. The exporter reads the live screen instead.

### 2.3 When a slice fails

`step()` returns `Failed`, leaves `error()` set, and never commits:

- a line in the range has left scrollback (index below zero), from the cap
  or a scrollback clear;
- `cols()` differs from its value at `open()`, because a resize reflows
  scrollback;
- a write returns fewer bytes than given, or `commit()` fails.

Lines pushed after `open()` are not exported. Destroying the exporter
before `Done` removes the temporary file, which is `QSaveFile`'s behaviour
without `commit()`.

### 2.4 `TerminalWidget` — `src/terminalwidget.h`, `src/terminalwidget.cpp`

```cpp
public:
    // Starts an export. False, with captureFailed emitted, when one is running
    // or open() fails.
    bool startExport(ScrollbackExporter::Request request);
signals:
    void exportFinished(const QString &path, bool ok);
```

- One export at a time per widget, held in a `std::unique_ptr` member.
- `startExport` sets the Html colours and font size from the widget. A
  caller sets the format, the path and, for Cast, the block.
- `runExportSlice` is the slice. `finishExport` releases the exporter and
  emits the signals.
- A slice calls `step(kExportLinesPerStep)` until it returns other than
  `More` or `kExportSliceMs` has passed on a `QElapsedTimer`. With `More`
  it schedules the next slice with `QTimer::singleShot(0, this, …)`.
- `kExportLinesPerStep` is 256 and `kExportSliceMs` is 8.
- On `Failed` it emits `captureFailed` with `error()`. On `Done` or
  `Failed` it emits `exportFinished` and releases the exporter.
- `TerminalWidget::recalcGridSize` fails a running export when it changes
  the grid's width.
  Two width changes before the next slice can leave `cols()` at its old
  value after two reflows, which the exporter's own check cannot see.
- Closing the tab destroys the widget and its exporter; no file is left and
  nothing is reported.
- `exportAsText`, `exportAsHtml` and `exportBlockAsCast` are removed. The
  three menu actions and `MainWindow::setupSettingsMenu` call `startExport`.
  `MainWindow` shows its "Scrollback exported to" message on
  `exportFinished` with `ok` true.
- Share Block passes the block's `id` and `commandTextAt`'s text.
- `outputTextAt` stays for Copy Output and `copyLastCommandOutput`; the
  clipboard needs the whole text.

## 3. Invariants

Fixtures feed `VtParser` into a `TerminalGrid`, as
`tests/features/osc133_rerun_safety` does. Expected bytes are worked out
from the pre-change function bodies and written into the test. The grid
stamps `commandStartMs` and `commandEndMs` from the wall clock, so a cast
fixture sets both to fixed values through `promptRegions()`.

- **INV-1** — Text output is byte-identical to `exportAsText`. The fixture
  has a line with trailing spaces, an empty line inside the text and empty
  lines at the end, and is driven with `step(1)`. Broken by dropping the
  trailing-empty-line trim, which only a sliced writer can get wrong
  across slices. *Test:* `tests/features/scrollback_export_streaming`.
- **INV-2** — Html output is byte-identical to `exportAsHtml`, including
  inverse, bold and a non-default background. Broken by a changed span
  rule. *Test:* `tests/features/scrollback_export_streaming`.
- **INV-3** — Cast output is byte-identical to `exportBlockAsCast`, for a
  block whose output starts in scrollback and ends on the screen, with a
  quote and a backslash in it. A tab from the parser moves the cursor and
  never reaches a cell, so it cannot be a fixture. Broken by escaping that differs across a slice
  boundary. *Test:* `tests/features/scrollback_export_streaming`.
- **INV-4** — The exporter writes as it goes. `bytesWritten()` rises across
  at least two `step(1)` calls that return `More`. Broken by building the
  whole export and writing it at the end. *Test:*
  `tests/features/scrollback_export_streaming`.
- **INV-5** — A line that leaves scrollback before it is written fails the
  export, and no file exists at `path`. The fixture calls
  `setMaxScrollback(1000)`, the minimum `setMaxScrollback` accepts, runs one
  `step(1)`, then feeds lines until an unwritten line is dropped. Broken by removing the offset check, which reads shifted
  lines and returns `Done`. *Test:*
  `tests/features/scrollback_export_streaming`.
- **INV-6** — Output that does not evict leaves the result unchanged. After
  one `step(1)`, the fixture feeds more lines under a large cap; the file
  equals INV-1's expected bytes. Broken by taking the range end from the
  live `scrollbackSize()`. *Test:*
  `tests/features/scrollback_export_streaming`.
- **INV-7** — A change of `cols()` fails the export. Broken by ignoring
  `resize`. *Test:* `tests/features/scrollback_export_streaming`.
- **INV-8** — An exporter destroyed before `Done` leaves no file at `path`.
  Broken by writing through `QFile`. *Test:*
  `tests/features/scrollback_export_streaming`.
- **INV-9** — No caller builds a whole export. `contextMenuEvent` and
  `MainWindow::setupSettingsMenu` call `startExport`, and none of
  `exportAsText`, `exportAsHtml` or `exportBlockAsCast` remains in
  `src/terminalwidget.h`, `src/terminalwidget.cpp` or `src/mainwindow.cpp`,
  comments included.
  Broken by any caller left on the old functions. *Test:*
  `tests/features/scrollback_export_streaming`, source scrape.
- **INV-10** — `startExport` runs in slices and reports. Its slice calls
  `step(`, checks `kExportSliceMs`, reschedules with `QTimer::singleShot`,
  emits `captureFailed` on `Failed`, and refuses while an export is running.
  `recalcGridSize` fails a running export when it changes the width.
  Broken by a loop that runs `step` to `Done` in one call, or by a resize
  that leaves the export running. *Test:*
  `tests/features/scrollback_export_streaming`, source scrape of
  `TerminalWidget::startExport` and its slice function.

## 4. RAM / build cost

The exporter holds the screen rows' copy, one line's formatted text and
`QSaveFile`'s write buffer. The change adds two source files to
`ants_vt_lib`. The test joins the `test_vt` bundle, which already has
`SRC_TERMINALWIDGET_PATH` and `ANTS_MAINWINDOW_SOURCES`; it adds a path
define for `src/terminalwidget.h`. `terminalwidget_export_safety` adds path
defines for the exporter's two files.

## 5. Out of scope

- The clipboard copies (Copy Output, `copyLastCommandOutput`, rich copy).
  The clipboard takes the whole text.
- A progress indicator or cancel button.
- Several exports at once in one tab.

## 6. Tests

Feature test: `tests/features/scrollback_export_streaming/`, in the
`test_vt` bundle. Covers INV-1, INV-2, INV-3, INV-4, INV-5, INV-6, INV-7,
INV-8, INV-9 and INV-10. Verify each test
fails against pre-change source first. The `ScrollbackExporter` tests fail to compile
there, since `ScrollbackExporter` does not exist; add a stub first so they
fail on assertions.

## 7. Cross-doc impact

- `tests/features/terminalwidget_export_safety` — INV-1 to INV-4 scrape
  the removed functions and the menu handlers. Move their write checks
  onto `ScrollbackExporter`.
- `tests/features/mainwindow_command_safety` INV-2 scrapes the
  `QSaveFile` write in `MainWindow`'s export action. It now checks the
  `startExport` call and the success message on `exportFinished`.
- `tests/features/session_capture_perms/spec.md` names `exportBlockAsCast`.
- Comments naming the removed functions: in `src/terminalwidget.h` above
  the `exportBlockAsCast` declaration, and in `src/terminalwidget.cpp`
  inside `copySelectionRich` and `contextMenuEvent`.
- `CHANGELOG.md` — a `### Fixed` entry.

## Cold-eyes loop log

| Loop | Date | Lanes | Q1 | Q2 | Q3 | Q4 | Outcome |
|---|---|---|---|---|---|---|---|
| 1 | 2026-09-17 | 1 (user decision: one reviewer, one pass) | 1 | 1 | 0 | 3 | Stopped at the user's one-pass cap: verified 5, fixed 5, dismissed 0. Q4: cast bytes carry wall-clock stamps, so the fixture pins commandStartMs/commandEndMs; setMaxScrollback clamps to 1000, so INV-5 names it; expected bytes come from the function bodies, since the old functions need a widget. Q2: INV-9 and section 7 disagreed on the comments naming the removed functions (copySelectionRich, contextMenuEvent). Q1 (from an open question): a cols() check misses two width changes in one turn, so recalcGridSize fails a running export. All five findings land on this new document. Mechanical pre-pass: spec_lint and check-doc-facts clean apart from two forward references. |
