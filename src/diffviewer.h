// diffviewer — Review Changes dialog (ANTS-1145, 0.7.73).
//
// Carved out of MainWindow::showDiffViewer (~430 LoC) as the
// simplest of the three L6 LoC carve-outs from the 2026-05-01
// indie-review sweep. The dialog runs five async git probes on a
// supplied working directory, renders coloured HTML, owns its own
// QFileSystemWatcher + debounce timer for live updates across git
// operations, preserves scroll position across refreshes, and
// supports a Copy-Diff payload covering all five sections.
//
// The dialog is parented to `parent` with WA_DeleteOnClose, so it
// destroys itself on close. The caller wires its own destroyed()
// connection if it needs to react to close (e.g. to re-enable a
// "Review Changes" button or refresh that button's enabled state).

#pragma once

#include <QString>

class QDialog;
class QWidget;

namespace diffviewer {

// Build and show the Review Changes dialog for `cwd`. Returns the
// dialog so the caller can connect to its destroyed() signal for
// post-close work. The dialog handles its own lifetime via
// WA_DeleteOnClose.
QDialog *show(QWidget *parent,
              const QString &cwd,
              const QString &themeName);

}  // namespace diffviewer
