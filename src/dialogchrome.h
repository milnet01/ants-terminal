// ANTS-1242 — DialogChrome
//
// Applies the ants frameless TitleBar to a QDialog so its chrome
// matches the active terminal theme. Without this, the dialog's
// title bar falls through to whatever KWin / GNOME / Wayland's
// system colour scheme is, ignoring Qt's QPalette — leaving a
// grey bar above an otherwise-themed dialog.
//
// Usage (from a dialog ctor, BEFORE any layout is set):
//
//     auto chrome = DialogChrome::install(this);
//     m_titleBar  = chrome.titleBar;     // keep for live theme refresh
//     QWidget *content = chrome.contentArea;
//     auto *root = new QVBoxLayout(content);   // not `new QVBoxLayout(this)`
//
// MainWindow MUST call DialogChrome::setActiveTheme(name) whenever
// the global theme changes so newly-opened dialogs inherit the
// correct colours.

#pragma once

#include <QString>

class QDialog;
class QWidget;
class TitleBar;

namespace DialogChrome {

struct InstallResult {
    TitleBar *titleBar;       // chrome bar, owned by `dlg`
    QWidget  *contentArea;    // parent widget for the dialog's own layouts
};

// Sets `Qt::FramelessWindowHint`, wires close/min/max signals to
// the dialog, and creates a content QWidget below the bar that the
// dialog should use as the layout parent. Applies `themeName` (or
// DialogChrome::activeTheme() when empty) to the bar + the
// dialog's QPalette. Returns nullptr fields if `dlg` is null.
InstallResult install(QDialog *dlg, const QString &themeName = QString());

// Re-applies the named theme to a chrome bar (and its parent
// dialog's QPalette). Use from a dialog that supports live theme
// changes — call this on the TitleBar* returned by install().
void applyTheme(QDialog *dlg, TitleBar *bar, const QString &themeName);

// MainWindow calls this on every theme change so future dialog
// installations pick up the new colours. The empty default keeps
// chrome installation safe before MainWindow has set anything.
void setActiveTheme(const QString &name);
QString activeTheme();

}  // namespace DialogChrome
