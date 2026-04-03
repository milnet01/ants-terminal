#pragma once

#include "config.h"
#include "themes.h"

#include <QMainWindow>
#include <QTabWidget>

class TitleBar;
class TerminalWidget;

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

private:
    void setupMenus();
    void applyTheme(const QString &name);
    void centerWindow();
    void changeFontSize(int delta);
    TerminalWidget *currentTerminal() const;

    TitleBar *m_titleBar = nullptr;
    QMenuBar *m_menuBar = nullptr;
    QTabWidget *m_tabWidget = nullptr;
    Config m_config;
    QString m_currentTheme;
};
