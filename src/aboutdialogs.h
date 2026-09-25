// ANTS-1181 — extracted from mainwindow.cpp's setupHelpMenu(). The two
// About dialogs are pure presentation: they read ANTS_VERSION + qVersion
// + (optionally) a Lua-version literal, ask ants-mcpd for its build line
// (ANTS-5340), and pop a non-modal QDialog with a single OK button. Wayland-friendly pattern (see comment in cpp).
//
// Free functions rather than QObject methods because they don't track
// any state past the QDialog::WA_DeleteOnClose lifecycle.

#pragma once

#include <QList>
#include <QString>

class QWidget;

namespace AboutDialogs {

// ANTS-5341 — one tab's title and the shells in it (a split tab has several),
// so a Claude Code session running an older ants-mcpd can be named by tab.
struct TabShells {
    QString title;
    QList<qint64> shellPids;
};

// Show the "About Ants Terminal" dialog. Parents to `parent` so the WM
// keeps it on top via xdg_toplevel transient_for; non-modal so Wayland's
// missing ApplicationModal doesn't drop OK clicks (QTBUG-79126).
void showAboutAnts(QWidget *parent, const QList<TabShells> &tabs = {});

// Show the "About Qt" dialog. Custom variant of QMessageBox::aboutQt
// that avoids the modal exec() Wayland silently breaks.
void showAboutQt(QWidget *parent);

}  // namespace AboutDialogs
