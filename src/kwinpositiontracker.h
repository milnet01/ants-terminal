#pragma once

#include <QPoint>
#include <QWidget>

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
