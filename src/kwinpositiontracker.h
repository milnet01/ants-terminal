#pragma once

#include <QByteArray>
#include <QPoint>
#include <QWidget>
#include <QtGlobal>

// ANTS-1142 — kwinPresent() is the guard runKWinScript
// (kwinpositiontracker.cpp) takes before it writes a script file or runs
// the dbus-send + kwin-script chain, so non-KDE compositors never orphan
// a script. Callers of runKWinScript need no check of their own.
//
// Detection mirrors Plasma's session manager: KDE_FULL_SESSION
// or XDG_CURRENT_DESKTOP containing "KDE". XDG_SESSION_TYPE is
// insufficient — KWin runs on both X11 and Wayland.
inline bool kwinPresent() {
    const QByteArray fullSession = qgetenv("KDE_FULL_SESSION");
    const QByteArray currentDesktop = qgetenv("XDG_CURRENT_DESKTOP");
    return fullSession == "true" || currentDesktop.contains("KDE");
}

// The KWin script that moves this process's window to (x, y). ANTS-1768 —
// the values are interpolated into JS unescaped, so every one is integral.
QString kwinMoveScript(qint64 pid, int x, int y);

// The KWin script that centres this process's window in its screen's
// placement area. The same ANTS-1768 integral-only rule applies.
QString kwinCenterScript(qint64 pid);

// The one KWin scripting chain: write `kwinJs` to an unpredictable temp file,
// then loadScript → start → unloadScript over dbus-send, removing the file on
// every path. `tag` names the temp file (kwin_<tag>_ants_*) and, with the pid,
// the registered script. A no-op when KWin is not the compositor. The async
// processes are parented to `owner`.
void runKWinScript(QObject *owner, const QString &kwinJs, const QString &tag);

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
    // windows), through runKWinScript().
    void setPosition(int x, int y);

private:
    QWidget *m_window;
    QPoint m_pos;
};
