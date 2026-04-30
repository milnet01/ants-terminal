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

// ANTS-1051: pseudo-modal blocking. Returns true iff the event
// should be SUPPRESSED — i.e. it's a mouse/key/wheel event landing
// outside any visible QDialog's tree. The 0.7.50 commit made all
// dialogs non-modal to dodge QTBUG-79126 (frameless+translucent
// parent + setModal(true) drops button clicks on Wayland); this
// helper restores the click-blocking-on-the-parent semantics
// manually.
//
// Returns true iff:
//   * event is non-null AND event->type() is one of
//     MouseButtonPress, MouseButtonRelease, MouseButtonDblClick,
//     Wheel, KeyPress, KeyRelease (the full mouse-and-keyboard
//     interactive set; KeyRelease pairs with KeyPress to prevent
//     Qt modifier-state desync), AND
//   * at least one QDialog is visible among the top-level widgets,
//     AND
//   * watched is a QWidget that is neither the dialog itself nor a
//     descendant of any visible dialog. Qt's QWidget::isAncestorOf
//     is strict (a widget is not its own ancestor), so the explicit
//     `watched == d` short-circuit is required to allow clicks on
//     the dialog's own frame.
inline bool shouldSuppressEventForDialog(QObject *watched, QEvent *event) {
    if (!event || !watched) return false;
    const QEvent::Type t = event->type();
    if (t != QEvent::MouseButtonPress &&
        t != QEvent::MouseButtonRelease &&
        t != QEvent::MouseButtonDblClick &&
        t != QEvent::Wheel &&
        t != QEvent::KeyPress &&
        t != QEvent::KeyRelease) {
        return false;
    }
    auto *target = qobject_cast<QWidget *>(watched);
    if (!target) return false;

    // Walk top-level widgets once and count visible dialogs;
    // also check whether `target` is inside any of them.
    bool anyDialogVisible = false;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *w : tops) {
        auto *d = qobject_cast<QDialog *>(w);
        if (!d || !d->isVisible()) continue;
        anyDialogVisible = true;
        if (target == d || d->isAncestorOf(target)) {
            // target is inside this dialog's tree — allow.
            return false;
        }
    }
    return anyDialogVisible;
}

}  // namespace dialogfocus
