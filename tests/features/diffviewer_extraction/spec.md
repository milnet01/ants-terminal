# DiffViewerDialog extraction (ANTS-1145)

Companion test for `docs/specs/ANTS-1145.md`. The full
contract — surface-design choice, behavioural-parity statement,
include-hygiene plan, per-test re-pointing table for the four
existing `review_changes_*` tests — lives in the spec; this file
documents only what `test_diffviewer_extraction.cpp` itself
asserts.

## Invariants pinned by this test

- **INV-1** `src/diffviewer.h` declares the exact signature
  `QDialog *show(QWidget *parent, const QString &cwd, const
  QString &themeName);`. Locks parameter order/types so a future
  "minor cleanup" PR can't silently reorder them.
- **INV-2a** `src/diffviewer.cpp` contains the structural markers:
  `reviewChangesDialog` objectName, `struct ProbeState`,
  `pending = 5`, `for-each-ref` git probe, `QFileSystemWatcher`,
  `runProbes` lambda name, `lastHtml` shared cache.
- **INV-2b** `src/diffviewer.cpp` contains every user-visible
  string byte-identical to the pre-extraction code: window title,
  button labels (Refresh / Close / Copy Diff), live-status texts,
  section headers, empty-state line.
- **INV-2c** `src/diffviewer.cpp` preserves sub-object names
  (`reviewLiveStatus`, `reviewRefreshBtn`) so feature-coverage
  QA helpers can find them.
- **INV-3a** `MainWindow::showDiffViewer` body is ≤ 50 meaningful
  LoC. "Meaningful" = physical lines between the function's
  opening and closing brace, exclusive of (a) blank lines and
  (b) lines whose first non-whitespace character is `//`.
- **INV-3b** `MainWindow::showDiffViewer` body contains
  `diffviewer::show(` — delegation present.
- **INV-4** `mainwindow.cpp` no longer contains `struct
  ProbeState`, `for-each-ref`, `runProbes`, or
  `reviewChangesDialog` — all moved to `diffviewer.cpp`.

INV-5 (re-pointing the four existing `review_changes_*` tests)
is verified externally by `ctest -R review_changes` reporting
4/4 pass post-extraction; not asserted here.
