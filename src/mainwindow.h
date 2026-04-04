#pragma once

#include "config.h"
#include "themes.h"

#include <QMainWindow>
#include <QTabWidget>

class TitleBar;
class TerminalWidget;
class QSplitter;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onTitleChanged(const QString &title);
    void onShellExited(int code);
    void onImagePasted(const QImage &image);
    void toggleMaximize();

    void newTab();
    void closeTab(int index);
    void closeCurrentTab();
    void onTabChanged(int index);

    // Split pane
    void splitHorizontal();
    void splitVertical();
    void closeFocusedPane();

private:
    void setupMenus();
    void applyTheme(const QString &name);
    void centerWindow();
    void changeFontSize(int delta);
    void applyFontSizeToAll(int size);
    TerminalWidget *currentTerminal() const;
    TerminalWidget *focusedTerminal() const;

    // Split helpers
    TerminalWidget *createTerminal();
    void connectTerminal(TerminalWidget *terminal);
    void splitCurrentPane(Qt::Orientation orientation);
    QSplitter *findParentSplitter(QWidget *w) const;
    void cleanupEmptySplitters(QWidget *tabRoot);

    // Apply settings from config to a terminal
    void applyConfigToTerminal(TerminalWidget *terminal);

    TitleBar *m_titleBar = nullptr;
    QMenuBar *m_menuBar = nullptr;
    QTabWidget *m_tabWidget = nullptr;
    Config m_config;
    QString m_currentTheme;
};
