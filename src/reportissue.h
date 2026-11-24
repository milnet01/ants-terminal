// ANTS-6911 — Help → Report an Issue. A small dialog asks whether this is a
// bug or an idea and for a one-line title, then opens GitHub's issue form in
// the browser with the title and the system details filled in. The person
// sends it from their own GitHub account: nothing is posted from here and no
// token is stored.
//
// A browser link cannot carry a file, so a log is never attached. When a debug
// log exists the dialog warns that it can hold private text and offers its
// folder, so the person can read it and attach it by hand.

#pragma once

#include <QString>
#include <QUrl>

class QDialog;
class QWidget;

namespace ReportIssue {

enum class Kind { Bug, Idea };

// The details filled into the form's `environment` field. All of it is shown
// in the dialog before anything is opened.
struct Environment {
    QString appVersion;  // ANTS_VERSION and the build commit
    QString qtVersion;   // the Qt runtime
    QString system;      // distribution and kernel
    QString desktop;     // desktop and session type (Wayland or X11)
    QString package;     // "AppImage" when run from one, else empty
    QString toText() const;
};

Environment currentEnvironment();

// The new-issue link for `kind`'s form (.github/ISSUE_TEMPLATE), with `title`
// and the `environment` field pre-filled.
QUrl issueUrl(Kind kind, const QString &title, const Environment &env);

// The dialog, not yet shown. `logPath` names the debug log; the log warning
// and the Open Log Folder button appear only when that file, or its rotated
// `.1` copy, exists.
QDialog *createDialog(QWidget *parent, const QString &logPath);

// createDialog, then show it non-modally (the Wayland pattern AboutDialogs
// documents).
void show(QWidget *parent, const QString &logPath);

}  // namespace ReportIssue
