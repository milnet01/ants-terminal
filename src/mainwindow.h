#pragma once

#include "config.h"
#include "themes.h"

#include <QMainWindow>

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

private:
    void setupMenus();
    void applyTheme(const QString &name);
    void centerWindow();
    void changeFontSize(int delta);

    TerminalWidget *m_terminal = nullptr;
    Config m_config;
    QString m_currentTheme;
};
