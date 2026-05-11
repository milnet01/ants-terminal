// ANTS-1242 — see dialogchrome.h for design notes.

#include "dialogchrome.h"

#include "themes.h"
#include "titlebar.h"

#include <QDialog>
#include <QPalette>
#include <QVBoxLayout>
#include <QWidget>

namespace DialogChrome {

namespace {
// Module-level cache so a dialog ctor doesn't need to receive the
// theme name as a parameter. MainWindow keeps this in sync via
// setActiveTheme() on every theme change.
QString g_activeTheme;
}  // namespace

void setActiveTheme(const QString &name) {
    g_activeTheme = name;
}

QString activeTheme() {
    return g_activeTheme;
}

void applyTheme(QDialog *dlg, TitleBar *bar, const QString &themeName) {
    if (!dlg && !bar) return;
    const Theme &th = Themes::byName(themeName);
    if (bar) {
        bar->setThemeColors(th.bgSecondary, th.textPrimary,
                            th.accent, th.border);
    }
    if (dlg) {
        QPalette dp = dlg->palette();
        dp.setColor(QPalette::Window, th.bgPrimary);
        dp.setColor(QPalette::WindowText, th.textPrimary);
        dp.setColor(QPalette::Base, th.bgPrimary);
        dp.setColor(QPalette::AlternateBase, th.bgSecondary);
        dp.setColor(QPalette::Text, th.textPrimary);
        dp.setColor(QPalette::ButtonText, th.textPrimary);
        dp.setColor(QPalette::Button, th.bgSecondary);
        dlg->setPalette(dp);
        dlg->setAutoFillBackground(true);
    }
}

InstallResult install(QDialog *dlg, const QString &themeName) {
    InstallResult r{nullptr, nullptr};
    if (!dlg) return r;

    dlg->setWindowFlag(Qt::FramelessWindowHint);
    dlg->setAttribute(Qt::WA_StyledBackground, true);

    auto *bar = new TitleBar(dlg);
    bar->setTitle(dlg->windowTitle());
    bar->centerButton()->hide();
    QObject::connect(bar, &TitleBar::closeRequested, dlg, &QDialog::reject);
    QObject::connect(bar, &TitleBar::minimizeRequested,
                     dlg, &QWidget::showMinimized);
    QObject::connect(bar, &TitleBar::maximizeRequested, dlg, [dlg]() {
        if (dlg->isMaximized()) dlg->showNormal();
        else dlg->showMaximized();
    });

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(bar);

    auto *content = new QWidget(dlg);
    root->addWidget(content, 1);

    applyTheme(dlg, bar, themeName.isEmpty() ? g_activeTheme : themeName);

    r.titleBar = bar;
    r.contentArea = content;
    return r;
}

}  // namespace DialogChrome
