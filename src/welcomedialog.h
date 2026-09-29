// ANTS-5558 — the first-run welcome dialog: thanks, a short feature list,
// one-click setup rows (ants::claude_setup), and three donation links.
// Contract: docs/specs/ANTS-5558-welcome-dialog.md.
//
// Object names the tests and the source checks read:
//   welcomeClaudeSection      the "Set up with Claude Code" group
//   welcomeClaudeMissingHint  the one-line hint shown instead of it
//   welcomeClaudeMdButton     the optional CLAUDE.md note row's button
//   welcomeShellButton        the shell-integration row's button
//   welcomePreview            the non-modal preview a writing row opens
//   welcomeDonateGitHub / welcomeDonatePatreon / welcomeDonatePayBru

#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>

class Config;

class WelcomeDialog : public QDialog {
    Q_OBJECT
public:
    // What the dialog reads from the machine, injectable for tests.
    struct Options {
        bool        claudeDetected = false;
        QStringList shellScriptDirs;   // the shell-integration search list
        QString     claudeMdPath;      // the CLAUDE.md the note row edits
    };

    // Reads the machine: Claude Code detection and the default paths.
    static Options detect();

    WelcomeDialog(const QString &themeName, const Options &opts,
                  QWidget *parent = nullptr);
};

namespace welcome {

// Calls `show` and sets `ui.welcome_shown` when the key is false or absent;
// does nothing when it is true. MainWindow calls it from its first show.
void maybeAutoShow(Config &cfg, const std::function<void()> &show);

}  // namespace welcome
