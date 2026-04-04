#include "mainwindow.h"

#include <QApplication>
#include <QFont>
#include <QSurfaceFormat>

int main(int argc, char *argv[]) {
    // Set default surface format with alpha for per-pixel transparency
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    fmt.setDepthBufferSize(24);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("Ants Terminal");
    app.setApplicationVersion("0.3.0");
    app.setStyle("Fusion");

    QFont font("Monospace", 11);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    app.setFont(font);

    MainWindow window;
    window.show();

    return app.exec();
}
