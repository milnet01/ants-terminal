#include "xcbpositiontracker.h"

#include <QProcess>
#include <QApplication>
#include <QDir>
#include <QFile>

XcbPositionTracker::XcbPositionTracker(QWidget *trackedWindow)
    : m_window(trackedWindow) {
}

void XcbPositionTracker::setPosition(int x, int y) {
    m_pos = QPoint(x, y);

    // Use KWin scripting to position the window — the only reliable
    // method for frameless windows on KDE Plasma compositor.
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

    QString scriptPath = QDir::tempPath() + "/kwin_pos_ants.js";
    {
        QFile f(scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(kwinJs.toUtf8());
    }

    auto *proc = new QProcess(m_window);
    proc->start("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.loadScript",
        QString("string:%1").arg(scriptPath),
        "string:ants_terminal_pos"
    });
    QObject::connect(proc, &QProcess::finished, m_window, [proc, scriptPath]() {
        proc->deleteLater();
        auto *proc2 = new QProcess(proc->parent());
        proc2->start("dbus-send", {
            "--session", "--dest=org.kde.KWin", "--print-reply",
            "/Scripting", "org.kde.kwin.Scripting.start"
        });
        QObject::connect(proc2, &QProcess::finished, proc2, [proc2, scriptPath]() {
            proc2->deleteLater();
            QProcess::startDetached("dbus-send", {
                "--session", "--dest=org.kde.KWin", "--print-reply",
                "/Scripting", "org.kde.kwin.Scripting.unloadScript",
                "string:ants_terminal_pos"
            });
            QFile::remove(scriptPath);
        });
    });
}
