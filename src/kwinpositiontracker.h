#pragma once

#include <QByteArray>
#include <QPoint>
#include <QWidget>
#include <QtGlobal>

// ANTS-1142 — kwinPresent() lifted from
// KWinPositionTracker::setPosition's inline guard so
// MainWindow::moveViaKWin and MainWindow::centerWindow (which
// fire the same dbus-send + kwin-script chain) can gate behind
// it instead of always running and orphaning /tmp scripts on
// non-KDE compositors.
//
// Detection mirrors Plasma's session manager: KDE_FULL_SESSION
// or XDG_CURRENT_DESKTOP containing "KDE". XDG_SESSION_TYPE is
// insufficient — KWin runs on both X11 and Wayland.
inline bool kwinPresent() {
    const QByteArray fullSession = qgetenv("KDE_FULL_SESSION");
    const QByteArray currentDesktop = qgetenv("XDG_CURRENT_DESKTOP");
    return fullSession == "true" || currentDesktop.contains("KDE");
}

// Tracks window position ourselves since Qt's pos()/moveEvent() are broken
// for frameless windows on KWin compositor.
// - Position is tracked by intercepting move() calls via updatePos()
// - Restore uses KWin scripting (the only reliable method on KDE)
//
// renamed from XcbPositionTracker (ANTS-1045) — the class doesn't use
// XCB at all; it talks to KWin's scripting D-Bus interface.
class KWinPositionTracker {
public:
    explicit KWinPositionTracker(QWidget *trackedWindow);
    ~KWinPositionTracker() = default;

    QPoint currentPos() const { return m_pos; }
    void updatePos(const QPoint &pos) { m_pos = pos; }

    // Set position via KWin scripting (only reliable method for frameless
    // windows). Bails early when KWin isn't the compositor (ANTS-1045
    // INV-4); on the failure path, a QScopeGuard ensures the temp script
    // doesn't leak (ANTS-1045 INV-5).
    void setPosition(int x, int y);

private:
    QWidget *m_window;
    QPoint m_pos;
};
