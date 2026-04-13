// Safe: QProcess with explicit args list, Qt event-loop exec().
#include <QProcess>
#include <QApplication>

void launch_ls() {
    QProcess p;
    p.start("ls", {"-la"});
}

int run_gui(int argc, char **argv) {
    QApplication app(argc, argv);
    return app.exec();  // Qt event loop — must not match cmd_injection
}
