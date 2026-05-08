// ANTS-1181 — extracted from mainwindow.cpp's setupHelpMenu(). The two
// About dialogs are pure presentation: they read ANTS_VERSION + qVersion
// + (optionally) a Lua-version literal and pop a non-modal QDialog with
// a single OK button. Wayland-friendly pattern (see comment in cpp).
//
// Free functions rather than QObject methods because they don't track
// any state past the QDialog::WA_DeleteOnClose lifecycle.

#pragma once

class QWidget;

namespace AboutDialogs {

// Show the "About Ants Terminal" dialog. Parents to `parent` so the WM
// keeps it on top via xdg_toplevel transient_for; non-modal so Wayland's
// missing ApplicationModal doesn't drop OK clicks (QTBUG-79126).
void showAboutAnts(QWidget *parent);

// Show the "About Qt" dialog. Custom variant of QMessageBox::aboutQt
// that avoids the modal exec() Wayland silently breaks.
void showAboutQt(QWidget *parent);

}  // namespace AboutDialogs
