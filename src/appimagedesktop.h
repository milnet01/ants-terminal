#pragma once

// Desktop integration for the AppImage. A downloaded AppImage has no menu
// entry, so the panel and task manager cannot match its window to an icon
// and show a generic one. On every launch from an AppImage, the terminal
// copies its bundled menu entry and icons under the user's data directory,
// with the entry's Exec pointing at the AppImage file. Each launch refreshes
// the path, so a moved or self-updated AppImage keeps a working entry.

#include <QByteArray>
#include <QString>

namespace AppImageDesktop {

// The menu entry's file name, as the distribution packages install it.
inline constexpr const char *kEntryName = "za.co.antsprojectshub.AntsTerminal.desktop";

// `entry` with each `Exec=ants-terminal…` line running `appImage` instead,
// and TryExec naming it. Returns an empty array when `appImage` holds a
// character the Exec quoting rules make awkward (" ` $ \ or a newline);
// the caller then skips integration.
QByteArray rewriteEntry(const QByteArray &entry, const QString &appImage);

// Writes the rewritten entry to <dataHome>/applications/ and each bundled
// hicolor PNG icon under <dataHome>/icons/hicolor/, reading both from the
// mounted `appDir`. A file whose content already matches is left alone.
// Returns false when the bundled entry is missing or a write fails.
bool integrate(const QString &appDir, const QString &appImage,
               const QString &dataHome);

// integrate() from $APPDIR, $APPIMAGE and $XDG_DATA_HOME (default
// ~/.local/share). Does nothing outside an AppImage.
void integrateFromEnvironment();

}  // namespace AppImageDesktop
