// ANTS-1181 — see aboutdialogs.h for rationale.

#include "aboutdialogs.h"

#include <QApplication>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QtCore/qglobal.h>

#ifndef ANTS_VERSION
#define ANTS_VERSION "0.0.0"
#endif

namespace {

// Heap-allocated, non-modal QDialog with plain QPushButton — mirrors the
// bg-tasks dialog pattern that does work under our KDE/KWin + Qt 6.11 +
// frameless+translucent parent on Wayland.
//
// Three prior fix attempts failed (0.7.22, 0.7.35, 0.7.49) — each
// diagnosed a symptom rather than the root cause. The actual cause:
// Qt's setModal(true) maps to Qt::ApplicationModal, which Wayland's
// xdg-shell protocol has no equivalent for (QTBUG-79126, QTBUG-90005).
// The "modal" status on Wayland results in click-event grabs that route
// the OK click into the modal-grab handler instead of the button —
// silently dropping it. Removing setModal() and using a plain
// QPushButton (no QDialogButtonBox role-dispatch) restores normal click
// delivery. The dialog is still parented to the MainWindow so the WM
// keeps it on top via xdg_toplevel transient_for, which is enough for
// the About-dialog use case (the user has no business modifying the
// parent while reading version info).
QDialog *makeAboutDialog(QWidget *parent,
                         const QString &windowTitle,
                         const QString &objectNameStem,
                         const QString &htmlBody) {
    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(windowTitle);
    dlg->setObjectName(objectNameStem + QStringLiteral("Dialog"));

    auto *layout = new QVBoxLayout(dlg);
    auto *label = new QLabel(htmlBody, dlg);
    label->setObjectName(objectNameStem + QStringLiteral("Body"));
    label->setTextFormat(Qt::RichText);
    // LinksAccessibleByMouse + LinksAccessibleByKeyboard only — no
    // TextSelectableByMouse, so the label doesn't grab focus or
    // intercept mouse events meant for the dialog frame / button.
    label->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                   | Qt::LinksAccessibleByKeyboard);
    label->setOpenExternalLinks(true);
    label->setWordWrap(true);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *okBtn = new QPushButton(QObject::tr("OK"), dlg);
    okBtn->setObjectName(objectNameStem + QStringLiteral("OkButton"));
    okBtn->setDefault(true);
    okBtn->setAutoDefault(true);
    QObject::connect(okBtn, &QPushButton::clicked, dlg, &QDialog::close);
    btnRow->addWidget(okBtn);

    layout->addWidget(label);
    layout->addLayout(btnRow);
    return dlg;
}

}  // namespace

namespace AboutDialogs {

void showAboutAnts(QWidget *parent) {
    const QString qtVer = QString::fromLatin1(qVersion());
    QString luaLine;
#ifdef ANTS_LUA_PLUGINS
    // lua.h is not included here (Lua is a plugin-layer detail). Ship
    // the engine version as a short literal kept in sync with the CMake
    // probe (`Lua ${LUA_VERSION}` line in the configure output). Matches
    // what PluginManager advertises.
    luaLine = QStringLiteral("<br/><b>Lua:</b> 5.4");
#endif
    const QString body = QStringLiteral(
        "<h3>Ants Terminal</h3>"
        "<p><b>Version:</b> %1<br/>"
        "<b>Qt runtime:</b> %2%3</p>"
        "<p>A modern, themeable terminal emulator with GPU "
        "rendering and Lua plugins. MIT-licensed.</p>"
        "<p><a href=\"https://github.com/milnet01/ants-terminal\">"
        "https://github.com/milnet01/ants-terminal</a></p>")
        .arg(QString::fromLatin1(ANTS_VERSION), qtVer, luaLine);

    auto *dlg = makeAboutDialog(parent,
                                QStringLiteral("About Ants Terminal"),
                                QStringLiteral("aboutAnts"),
                                body);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void showAboutQt(QWidget *parent) {
    // Custom dialog rather than QMessageBox::aboutQt: aboutQt wraps the
    // dialog in a modal exec() which Wayland doesn't honour
    // (QTBUG-79126), so OK clicks were silently dropped. Same
    // plain-QPushButton + non-modal pattern as About Ants.
    const QString body = QStringLiteral(
        "<h3>About Qt</h3>"
        "<p>Ants Terminal uses the Qt application framework.</p>"
        "<p><b>Qt runtime:</b> %1<br/>"
        "<b>Qt build:</b> %2</p>"
        "<p>Qt is licensed under multiple licenses including the GNU "
        "LGPL version 3 and the Qt Commercial License.</p>"
        "<p><a href=\"https://www.qt.io/licensing/\">"
        "https://www.qt.io/licensing/</a></p>")
        .arg(QString::fromLatin1(qVersion()),
             QString::fromLatin1(QT_VERSION_STR));

    auto *dlg = makeAboutDialog(parent,
                                QStringLiteral("About Qt"),
                                QStringLiteral("aboutQt"),
                                body);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

}  // namespace AboutDialogs
