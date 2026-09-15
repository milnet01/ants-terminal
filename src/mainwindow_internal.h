#pragma once

// ANTS-1677 — helpers shared by the files of MainWindow's source list,
// ANTS_MAINWINDOW_SOURCES_REL in CMakeLists.txt. Include it only from those
// files; tests/features/split_sources holds that line. This header declares;
// every function and constant is defined in a .cpp of the list. The one type
// here is defined in the header, because every file using it needs the
// definition.

#include "resolvedroot.h"

#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QMenu>
#include <QObject>
#include <QString>

#include <sys/types.h>

class QTabWidget;
class QWidget;
class TerminalWidget;

namespace mainwindowdetail {

// The MCP "remote control unavailable" envelope, defined in mainwindow.cpp.
extern const char *const kRcUnavailable;

// Defined in mainwindow.cpp.
QString firstNonShellDescendant(pid_t shellPid);
QString sourceToString(ants::ResolvedRoot::Source s);
TerminalWidget *activeTerminalInTab(QWidget *root);
QWidget *tabPageOf(const QTabWidget *tabs, QWidget *w);

// ANTS-1982 — keep a menu open after toggling a NON-exclusive checkable item, so the
// user can flip several independent checkboxes (Session Logging, Visual
// Bell, Background Blur…) in one visit instead of the menu dismissing on
// the first click. Exclusive/radio group members (Themes, Opacity,
// Scrollback) keep Qt's default "pick one and close" — the
// actionGroup()->isExclusive() guard distinguishes a checkbox from a
// radio. No Q_OBJECT needed: only the virtual eventFilter() is used.
class StayOpenOnToggleFilter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::MouseButtonRelease) {
            if (auto *menu = qobject_cast<QMenu *>(obj)) {
                QAction *a = menu->activeAction();
                if (a && a->isEnabled() && a->isCheckable()
                    && !(a->actionGroup() && a->actionGroup()->isExclusive())) {
                    a->trigger();   // toggle checked state + fire triggered()
                    return true;    // swallow the release so the menu stays open
                }
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

}  // namespace mainwindowdetail
