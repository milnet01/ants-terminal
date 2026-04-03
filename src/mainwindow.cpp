#include "mainwindow.h"
#include "terminalwidget.h"

#include <QApplication>
#include <QCloseEvent>
#include <QMenuBar>
#include <QStatusBar>
#include <QScreen>
#include <QFileDialog>
#include <QMessageBox>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Ants Terminal");

    // Restore window geometry
    int w = m_config.windowWidth();
    int h = m_config.windowHeight();
    resize(w, h);
    int x = m_config.windowX();
    int y = m_config.windowY();
    if (x >= 0 && y >= 0) {
        move(x, y);
    }

    // Terminal widget as central
    m_terminal = new TerminalWidget(this);
    setCentralWidget(m_terminal);

    connect(m_terminal, &TerminalWidget::titleChanged, this, &MainWindow::onTitleChanged);
    connect(m_terminal, &TerminalWidget::shellExited, this, &MainWindow::onShellExited);
    connect(m_terminal, &TerminalWidget::imagePasted, this, &MainWindow::onImagePasted);

    setupMenus();

    // Apply saved theme
    applyTheme(m_config.theme());

    // Apply saved font size
    // (font size is managed at widget level via theme application)

    // Start the shell
    if (!m_terminal->startShell()) {
        statusBar()->showMessage("Failed to start shell!");
    }

    m_terminal->setFocus();
}

void MainWindow::setupMenus() {
    QMenuBar *mb = menuBar();

    // File menu
    QMenu *fileMenu = mb->addMenu("&File");

    QAction *newWindowAction = fileMenu->addAction("&New Window");
    newWindowAction->setShortcut(QKeySequence("Ctrl+Shift+N"));
    connect(newWindowAction, &QAction::triggered, this, []() {
        auto *win = new MainWindow();
        win->show();
    });

    fileMenu->addSeparator();

    QAction *exitAction = fileMenu->addAction("E&xit");
    exitAction->setShortcut(QKeySequence("Ctrl+Shift+Q"));
    connect(exitAction, &QAction::triggered, this, &QWidget::close);

    // View menu
    QMenu *viewMenu = mb->addMenu("&View");

    // Themes submenu
    QMenu *themesMenu = viewMenu->addMenu("&Themes");
    for (const QString &name : Themes::names()) {
        QAction *a = themesMenu->addAction(name);
        connect(a, &QAction::triggered, this, [this, name]() {
            applyTheme(name);
        });
    }

    viewMenu->addSeparator();

    QAction *zoomIn = viewMenu->addAction("Zoom &In");
    zoomIn->setShortcut(QKeySequence("Ctrl+="));
    connect(zoomIn, &QAction::triggered, this, [this]() { changeFontSize(1); });

    QAction *zoomOut = viewMenu->addAction("Zoom &Out");
    zoomOut->setShortcut(QKeySequence("Ctrl+-"));
    connect(zoomOut, &QAction::triggered, this, [this]() { changeFontSize(-1); });

    QAction *zoomReset = viewMenu->addAction("&Reset Zoom");
    zoomReset->setShortcut(QKeySequence("Ctrl+0"));
    connect(zoomReset, &QAction::triggered, this, [this]() {
        m_config.setFontSize(11);
        applyTheme(m_currentTheme); // Re-apply to update font
    });

    viewMenu->addSeparator();

    QAction *centerAction = viewMenu->addAction("&Center Window");
    centerAction->setShortcut(QKeySequence("Ctrl+Shift+C"));
    connect(centerAction, &QAction::triggered, this, &MainWindow::centerWindow);
}

void MainWindow::applyTheme(const QString &name) {
    m_currentTheme = name;
    m_config.setTheme(name);

    const Theme &theme = Themes::byName(name);

    // Set window stylesheet for menu bar and status bar
    QString ss = QString(
        "QMainWindow { background-color: %1; }"
        "QMenuBar { background-color: %2; color: %3; border-bottom: 1px solid %4; }"
        "QMenuBar::item:selected { background-color: %5; border-radius: 4px; }"
        "QMenu { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        "QMenu::item { padding: 6px 24px 6px 12px; border-radius: 4px; }"
        "QMenu::item:selected { background-color: %5; color: %1; }"
        "QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }"
        "QStatusBar { background-color: %2; color: %6; border-top: 1px solid %4; }"
    ).arg(theme.bgPrimary.name(),
          theme.bgSecondary.name(),
          theme.textPrimary.name(),
          theme.border.name(),
          theme.accent.name(),
          theme.textSecondary.name());

    setStyleSheet(ss);

    // Apply colors to terminal
    m_terminal->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor);

    statusBar()->showMessage(QString("Theme: %1").arg(name), 3000);
}

void MainWindow::centerWindow() {
    if (QScreen *screen = this->screen()) {
        QRect geo = screen->availableGeometry();
        move(geo.x() + (geo.width() - width()) / 2,
             geo.y() + (geo.height() - height()) / 2);
    }
}

void MainWindow::changeFontSize(int delta) {
    int size = m_config.fontSize() + delta;
    size = qBound(8, size, 32);
    m_config.setFontSize(size);
    // The terminal widget manages its own font — we need to tell it
    // For now, font size changes require restarting or we add a method
    // TODO: Add setFontSize to TerminalWidget
    statusBar()->showMessage(QString("Font size: %1pt (restart to apply)").arg(size), 3000);
}

void MainWindow::onTitleChanged(const QString &title) {
    if (title.isEmpty()) {
        setWindowTitle("Ants Terminal");
    } else {
        setWindowTitle(title + " — Ants Terminal");
    }
}

void MainWindow::onShellExited(int code) {
    statusBar()->showMessage(QString("Shell exited with code %1").arg(code));
}

void MainWindow::onImagePasted(const QImage &image) {
    // Save to a temporary file and display the path
    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                  + "/ants-terminal";
    QDir().mkpath(dir);
    QString filename = dir + "/paste_"
                       + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
                       + ".png";
    if (image.save(filename)) {
        statusBar()->showMessage(QString("Image saved: %1").arg(filename), 5000);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    m_config.setWindowGeometry(x(), y(), width(), height());
    event->accept();
}
