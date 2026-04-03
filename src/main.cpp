#include "mainwindow.h"

#include <QApplication>
#include <QFont>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Ants Terminal");
    app.setApplicationVersion("0.2.0");
    app.setStyle("Fusion");

    QFont font("Monospace", 11);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    app.setFont(font);

    MainWindow window;
    window.show();

    return app.exec();
}
