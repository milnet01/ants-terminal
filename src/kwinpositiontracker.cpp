// renamed from XcbPositionTracker (ANTS-1045) — KWin-D-Bus, not XCB.

#include "kwinpositiontracker.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryFile>

KWinPositionTracker::KWinPositionTracker(QWidget *trackedWindow)
    : m_window(trackedWindow) {
}

void KWinPositionTracker::setPosition(int x, int y) {
    m_pos = QPoint(x, y);

    // ANTS-1045 INV-4: bail before any temp-file write when KWin
    // isn't the active compositor. The KWin-script + dbus-send chain
    // only works under KDE Plasma; on GNOME/Mutter, Sway, Hyprland,
    // etc. the dbus call goes nowhere and the temp file used to leak.
    // Detection lifted into the kwinPresent() free function in
    // kwinpositiontracker.h (ANTS-1142 0.7.69) so MainWindow's
    // sister moveViaKWin / centerWindow paths can share the same
    // guard.
    if (!kwinPresent()) return;

    qint64 pid = QApplication::applicationPid();
    QString kwinJs = QString(
        "var clients = workspace.windowList();\n"
        "for (var i = 0; i < clients.length; i++) {\n"
        "    var c = clients[i];\n"
        "    if (c.pid === %1) {\n"
        "        c.frameGeometry = {\n"
        "            x: %2,\n"
        "            y: %3,\n"
        "            width: c.frameGeometry.width,\n"
        "            height: c.frameGeometry.height\n"
        "        };\n"
        "        break;\n"
        "    }\n"
        "}\n"
    ).arg(pid).arg(x).arg(y);

    // ANTS-1045 INV-5 / INV-5b: scope-guarded temp-file cleanup,
    // declared BEFORE the QTemporaryFile open so a buried-guard
    // regression can't bypass it (line-ordering check). The guard
    // runs QFile::remove(scriptPath) on every exit path that
    // doesn't make it to the dismiss() below; the dbus-send chain
    // owns cleanup once async dispatch starts.
    QString scriptPath;
    auto cleanup = qScopeGuard([&]{
        if (!scriptPath.isEmpty()) QFile::remove(scriptPath);
    });

    // Unpredictable tempfile name via QTemporaryFile so two Ants
    // instances can't stomp each other's KWin-script load and a
    // same-UID attacker can't pre-create a symlink at the predictable
    // path. `setAutoRemove(false)` because the async dbus-send chain
    // reads the file after this function returns; the QScopeGuard
    // above handles the remove on synchronous-failure paths instead.
    // 0.7.12 /indie-review TOCTOU fix preserved (INV-6).
    {
        QTemporaryFile f(QDir::tempPath() + "/kwin_pos_ants_XXXXXX.js");
        f.setAutoRemove(false);
        if (!f.open()) return;
        f.write(kwinJs.toUtf8());
        scriptPath = f.fileName();
    }

    auto *proc = new QProcess(m_window);
    proc->start("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.loadScript",
        QString("string:%1").arg(scriptPath),
        "string:ants_terminal_pos"
    });
    QObject::connect(proc, &QProcess::finished, m_window,
                     [proc, scriptPath](int exitCode,
                                         QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status != QProcess::NormalExit || exitCode != 0) {
            // First dbus-send failed (KWin service not responding,
            // bus unreachable, etc.). Remove the temp file
            // explicitly — the success-chain below won't fire.
            QFile::remove(scriptPath);
            return;
        }
        auto *proc2 = new QProcess(proc->parent());
        proc2->start("dbus-send", {
            "--session", "--dest=org.kde.KWin", "--print-reply",
            "/Scripting", "org.kde.kwin.Scripting.start"
        });
        QObject::connect(proc2, &QProcess::finished, proc2,
                         [proc2, scriptPath]() {
            proc2->deleteLater();
            QProcess::startDetached("dbus-send", {
                "--session", "--dest=org.kde.KWin", "--print-reply",
                "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                "string:ants_terminal_pos"
            });
            QFile::remove(scriptPath);
        });
    });
    QObject::connect(proc, &QProcess::errorOccurred, m_window,
                     [scriptPath](QProcess::ProcessError) {
        // dbus-send failed to even start (PATH empty, binary
        // missing, etc.) — the `finished` signal won't fire, so
        // remove explicitly here.
        QFile::remove(scriptPath);
    });

    // The async chain owns cleanup from here. Dismiss the guard so
    // the synchronous fallthrough doesn't double-remove a file the
    // dbus-send chain might still legitimately be reading.
    cleanup.dismiss();
}
