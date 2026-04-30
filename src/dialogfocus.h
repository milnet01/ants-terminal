#pragma once

// ANTS-1050: pure-logic helper that decides whether a Close event on
// `watched` should trigger a deferred focus-restore to the active
// terminal. Lives in its own header so the feature test
// (tests/features/dialog_close_focus_return/) can include and drive
// it without pulling in mainwindow.h's transitive Ants-headers
// blast radius. See spec INV-2 / 2a-d.

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QObject>
#include <QWidget>

namespace dialogfocus {

// Returns true iff a refocus to the active terminal is warranted:
//   * event is non-null AND event->type() == QEvent::Close
//   * watched is a QDialog subclass
//   * after this dialog closes, no other QDialog is still visible
//     (so a stacked-dialog close doesn't refocus through the
//     remaining dialog).
inline bool shouldRefocusOnDialogClose(QObject *watched, QEvent *event) {
    if (!event || event->type() != QEvent::Close) return false;
    auto *dlg = qobject_cast<QDialog *>(watched);
    if (!dlg) return false;
    // Stacked-dialog case: another QDialog is still visible after
    // this one closes — skip the refocus so the still-open dialog
    // isn't robbed of focus mid-keystroke. We exclude `dlg` itself
    // from the visibility check since QDialog::isVisible is still
    // true at the moment Close fires (the actual hide happens in
    // the Close handler chain).
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *w : tops) {
        if (w == dlg) continue;
        auto *other = qobject_cast<QDialog *>(w);
        if (other && other->isVisible()) return false;
    }
    return true;
}

}  // namespace dialogfocus
