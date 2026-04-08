#pragma once

#include <QPoint>
#include <QWidget>

// Tracks window position ourselves since Qt's pos()/moveEvent() are broken
// for frameless windows on KWin compositor.
// - Position is tracked by intercepting move() calls via updatePos()
// - Restore uses KWin scripting (the only reliable method on KDE)
class XcbPositionTracker {
public:
    explicit XcbPositionTracker(QWidget *trackedWindow);
    ~XcbPositionTracker() = default;

    QPoint currentPos() const { return m_pos; }
    void updatePos(const QPoint &pos) { m_pos = pos; }

    // Set position via KWin scripting (only reliable method for frameless windows)
    void setPosition(int x, int y);

private:
    QWidget *m_window;
    QPoint m_pos;
};
