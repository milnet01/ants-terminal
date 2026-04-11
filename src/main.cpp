#include "mainwindow.h"

#include <QApplication>
#include <QFont>
#include <QSurfaceFormat>
#include <QStringList>
#include <QIcon>
#include <QDir>

#include <csignal>

int main(int argc, char *argv[]) {
    // Ignore SIGPIPE — writing to a closed PTY delivers SIGPIPE which would
    // terminate the process.  Qt handles write errors via return codes.
    std::signal(SIGPIPE, SIG_IGN);
    // Set default surface format with alpha for per-pixel transparency
    // Do NOT set Core Profile here — it breaks QPainter's GL paint engine
    // font scaling on displays where physical DPI differs from logical DPI.
    // The GlRenderer requests Core Profile on its own context when needed.
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    fmt.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("Ants Terminal");
    app.setApplicationVersion("0.4.0");
    app.setStyle("Fusion");

    // Application icon (taskbar, window manager, dialogs)
    QIcon appIcon = QIcon::fromTheme("ants-terminal");
    if (appIcon.isNull()) {
        // Fallback: load from assets/ next to the executable
        QString base = QApplication::applicationDirPath() + "/../assets/ants-terminal";
        for (int sz : {16, 32, 48, 64, 128, 256})
            appIcon.addFile(QString("%1-%2.png").arg(base).arg(sz), QSize(sz, sz));
    }
    app.setWindowIcon(appIcon);

    // Check for --quake / --dropdown flag
    QStringList args = app.arguments();
    bool quakeMode = args.contains("--quake") || args.contains("--dropdown");

    QFont font("Monospace", 11);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    app.setFont(font);

    MainWindow window(quakeMode);
    window.show();

    return app.exec();
}
