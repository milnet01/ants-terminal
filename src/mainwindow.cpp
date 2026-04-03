#include "mainwindow.h"
#include "terminalwidget.h"
#include "titlebar.h"

#include <QApplication>
#include <QCloseEvent>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolButton>
#include <QScreen>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QProcess>
#include <QFile>
#include <QVBoxLayout>
#include <QTabBar>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("Ants Terminal");
    setWindowFlag(Qt::FramelessWindowHint);

    // Restore window geometry
    int w = m_config.windowWidth();
    int h = m_config.windowHeight();
    resize(w, h);
    int x = m_config.windowX();
    int y = m_config.windowY();
    if (x >= 0 && y >= 0) {
        move(x, y);
    }

    // Custom title bar
    m_titleBar = new TitleBar(this);
    m_titleBar->setTitle("Ants Terminal");
    connect(m_titleBar, &TitleBar::minimizeRequested, this, &QWidget::showMinimized);
    connect(m_titleBar, &TitleBar::maximizeRequested, this, &MainWindow::toggleMaximize);
    connect(m_titleBar, &TitleBar::closeRequested, this, &QWidget::close);
    connect(m_titleBar->centerButton(), &QToolButton::clicked, this, &MainWindow::centerWindow);

    // Standalone menu bar (NOT QMainWindow::menuBar(), to control layout order)
    m_menuBar = new QMenuBar(this);

    // Tab widget
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    // Layout: title bar → menu bar → tabs (with terminal content)
    QWidget *central = new QWidget(this);
    QVBoxLayout *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);
    vbox->addWidget(m_titleBar);
    vbox->addWidget(m_menuBar);
    vbox->addWidget(m_tabWidget, 1);
    setCentralWidget(central);

    setupMenus();

    // Apply saved theme
    applyTheme(m_config.theme());

    // Create first tab
    newTab();
}

void MainWindow::setupMenus() {
    // File menu
    QMenu *fileMenu = m_menuBar->addMenu("&File");

    QAction *newTabAction = fileMenu->addAction("New &Tab");
    newTabAction->setShortcut(QKeySequence("Ctrl+Shift+T"));
    connect(newTabAction, &QAction::triggered, this, &MainWindow::newTab);

    QAction *closeTabAction = fileMenu->addAction("&Close Tab");
    closeTabAction->setShortcut(QKeySequence("Ctrl+Shift+W"));
    connect(closeTabAction, &QAction::triggered, this, &MainWindow::closeCurrentTab);

    fileMenu->addSeparator();

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
    QMenu *viewMenu = m_menuBar->addMenu("&View");

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
        applyTheme(m_currentTheme);
    });

    viewMenu->addSeparator();

    QAction *centerAction = viewMenu->addAction("&Center Window");
    centerAction->setShortcut(QKeySequence("Ctrl+Shift+M"));
    connect(centerAction, &QAction::triggered, this, &MainWindow::centerWindow);
}

void MainWindow::newTab() {
    auto *terminal = new TerminalWidget(this);

    // Apply current theme to new tab
    if (!m_currentTheme.isEmpty()) {
        const Theme &theme = Themes::byName(m_currentTheme);
        terminal->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor);
    }

    connect(terminal, &TerminalWidget::titleChanged, this, [this, terminal](const QString &title) {
        int idx = m_tabWidget->indexOf(terminal);
        if (idx >= 0) {
            QString tabTitle = title.isEmpty() ? "Shell" : title;
            // Truncate long titles for the tab
            if (tabTitle.length() > 30)
                tabTitle = tabTitle.left(27) + "...";
            m_tabWidget->setTabText(idx, tabTitle);
        }
        // Update window title if this is the active tab
        if (terminal == currentTerminal()) {
            onTitleChanged(title);
        }
    });

    connect(terminal, &TerminalWidget::shellExited, this, [this, terminal](int code) {
        int idx = m_tabWidget->indexOf(terminal);
        if (idx >= 0) {
            closeTab(idx);
        }
    });

    connect(terminal, &TerminalWidget::imagePasted, this, &MainWindow::onImagePasted);

    int idx = m_tabWidget->addTab(terminal, "Shell");
    m_tabWidget->setCurrentIndex(idx);

    if (!terminal->startShell()) {
        statusBar()->showMessage("Failed to start shell!");
    }

    terminal->setFocus();

    // Hide tab bar when only one tab
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);
}

void MainWindow::closeTab(int index) {
    if (m_tabWidget->count() <= 1) {
        // Last tab — close the window
        close();
        return;
    }

    QWidget *w = m_tabWidget->widget(index);
    m_tabWidget->removeTab(index);
    w->deleteLater();

    // Hide tab bar when only one tab left
    m_tabWidget->tabBar()->setVisible(m_tabWidget->count() > 1);

    // Focus the new current terminal
    if (auto *t = currentTerminal()) {
        t->setFocus();
    }
}

void MainWindow::closeCurrentTab() {
    closeTab(m_tabWidget->currentIndex());
}

void MainWindow::onTabChanged(int /*index*/) {
    if (auto *t = currentTerminal()) {
        t->setFocus();
        onTitleChanged(t->shellTitle());
    }
}

TerminalWidget *MainWindow::currentTerminal() const {
    return qobject_cast<TerminalWidget *>(m_tabWidget->currentWidget());
}

void MainWindow::applyTheme(const QString &name) {
    m_currentTheme = name;
    m_config.setTheme(name);

    const Theme &theme = Themes::byName(name);

    // Style the menu bar, tab widget, and status bar
    QString ss = QString(
        "QMainWindow { background-color: %1; }"
        "QMenuBar { background-color: %2; color: %3; border-bottom: 1px solid %4; }"
        "QMenuBar::item:selected { background-color: %5; border-radius: 4px; }"
        "QMenu { background-color: %2; color: %3; border: 1px solid %4; padding: 4px; }"
        "QMenu::item { padding: 6px 24px 6px 12px; border-radius: 4px; }"
        "QMenu::item:selected { background-color: %5; color: %1; }"
        "QMenu::separator { height: 1px; background: %4; margin: 4px 8px; }"
        "QStatusBar { background-color: %2; color: %6; border-top: 1px solid %4; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar { background-color: %2; }"
        "QTabBar::tab { background-color: %2; color: %6; padding: 6px 16px;"
        "  border: none; border-bottom: 2px solid transparent; }"
        "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %5; }"
        "QTabBar::tab:hover { background-color: %1; color: %3; }"
        "QTabBar::close-button { image: none; }"
        "QTabBar::close-button:hover { background-color: #e74856; border-radius: 3px; }"
    ).arg(theme.bgPrimary.name(),
          theme.bgSecondary.name(),
          theme.textPrimary.name(),
          theme.border.name(),
          theme.accent.name(),
          theme.textSecondary.name());

    setStyleSheet(ss);

    // Style the custom title bar
    m_titleBar->setThemeColors(theme.bgSecondary, theme.textPrimary,
                                theme.accent, theme.border);

    // Apply colors to all terminal tabs
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto *t = qobject_cast<TerminalWidget *>(m_tabWidget->widget(i))) {
            t->applyThemeColors(theme.textPrimary, theme.bgPrimary, theme.cursor);
        }
    }

    statusBar()->showMessage(QString("Theme: %1").arg(name), 3000);
}

void MainWindow::centerWindow() {
    // Try Qt move() first (works on X11)
    if (QScreen *screen = this->screen()) {
        QRect geo = screen->availableGeometry();
        move(geo.x() + (geo.width() - width()) / 2,
             geo.y() + (geo.height() - height()) / 2);
    }

    // On Wayland, move() is ignored — use KWin scripting via D-Bus
    qint64 pid = QApplication::applicationPid();
    QString kwinJs = QStringLiteral(
        "var clients = workspace.windowList();\n"
        "for (var i = 0; i < clients.length; i++) {\n"
        "    var c = clients[i];\n"
        "    if (c.pid === %1) {\n"
        "        var area = workspace.clientArea(workspace.PlacementArea, c);\n"
        "        c.frameGeometry = {\n"
        "            x: area.x + Math.round((area.width - c.frameGeometry.width) / 2),\n"
        "            y: area.y + Math.round((area.height - c.frameGeometry.height) / 2),\n"
        "            width: c.frameGeometry.width,\n"
        "            height: c.frameGeometry.height\n"
        "        };\n"
        "        break;\n"
        "    }\n"
        "}\n"
    ).arg(pid);

    QString scriptPath = QDir::tempPath() + "/kwin_center_ants.js";
    {
        QFile f(scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(kwinJs.toUtf8());
    }

    QProcess::execute("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.loadScript",
        QStringLiteral("string:%1").arg(scriptPath),
        "string:ants_terminal_center"
    });
    QProcess::execute("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.start"
    });
    QProcess::execute("dbus-send", {
        "--session", "--dest=org.kde.KWin", "--print-reply",
        "/Scripting", "org.kde.kwin.Scripting.unloadScript",
        "string:ants_terminal_center"
    });

    QFile::remove(scriptPath);
}

void MainWindow::toggleMaximize() {
    if (isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void MainWindow::changeFontSize(int delta) {
    int size = m_config.fontSize() + delta;
    size = qBound(8, size, 32);
    m_config.setFontSize(size);
    statusBar()->showMessage(QString("Font size: %1pt (restart to apply)").arg(size), 3000);
}

void MainWindow::onTitleChanged(const QString &title) {
    QString windowTitle;
    if (title.isEmpty()) {
        windowTitle = "Ants Terminal";
    } else {
        windowTitle = title + " \u2014 Ants Terminal";
    }
    setWindowTitle(windowTitle);
    m_titleBar->setTitle(windowTitle);
}

void MainWindow::onShellExited(int code) {
    statusBar()->showMessage(QString("Shell exited with code %1").arg(code));
}

void MainWindow::onImagePasted(const QImage &image) {
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
