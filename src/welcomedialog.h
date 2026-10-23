// ANTS-5558 — the first-run welcome dialog: thanks, a short feature list,
// one-click setup rows (ants::claude_setup), and three donation links.
// Contract: docs/specs/ANTS-5558-welcome-dialog.md.
//
// Object names the tests and the source checks read:
//   welcomeClaudeSection      the "Set up with Claude Code" group
//   welcomeInstallClaudeButton  the Install Claude Code button shown instead
//   welcomeClaudePlanNote     the paid-plan note beside it (spec § 2.8)
//   welcomeClaudeMdButton     the optional CLAUDE.md note row's button
//   welcomeShellButton        the shell-integration row's button
//   welcomePreview            the non-modal preview a writing row opens
//   welcomeDonateGitHub / welcomeDonatePatreon / welcomeDonatePayBru

#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

class Config;
class QVBoxLayout;

namespace ants::claude_setup {
struct Outcome;
struct Status;
}

class WelcomeDialog : public QDialog {
    Q_OBJECT
public:
    // What the dialog reads from the machine, injectable for tests.
    struct Options {
        // Called at construction and on each window activation; empty
        // counts as not detected.
        std::function<bool()> detectClaude;
        // Opens a tab running `command`; empty disables the install button.
        std::function<void(const QString &)> openInstallTab;
        QStringList shellScriptDirs;   // the shell-integration search list
        QString     claudeMdPath;      // the CLAUDE.md the note row edits
    };

    // Reads the machine: Claude Code detection and the default paths.
    static Options detect();

    WelcomeDialog(const QString &themeName, const Options &opts,
                  QWidget *parent = nullptr);

protected:
    // Re-reads Claude Code detection on QEvent::WindowActivate (INV-13).
    bool event(QEvent *e) override;

private:
    // One setup row: a title, the current state, and an Install/Reinstall
    // button (defined in the .cpp).
    struct Row;
    using RowAction = std::function<void(const Row &)>;

    // Shows the setup section or the install offer; when detection turns
    // true, every setup row re-reads its state.
    void applyDetection();

    // Adds a row to `into` and reads its state; returns a re-read of it
    // for when detection turns true (INV-13).
    std::function<void()> addRow(QVBoxLayout *into, QWidget *owner,
                                 const QString &title, const QString &buttonName,
                                 std::function<ants::claude_setup::Status()> status,
                                 RowAction onClick);
    // Runs `action` at once and shows its result in the row.
    static RowAction runNow(std::function<ants::claude_setup::Outcome()> action);
    // Shows the exact change first; `action` runs only on accept (INV-9).
    RowAction runAfterPreview(std::function<QString()> preview,
                              std::function<ants::claude_setup::Outcome()> action);
    // A non-modal preview owned by this dialog; `onAccept` runs only from
    // its accept handler (INV-9, INV-12).
    void openPreview(const QString &title, const QString &intro, const QString &text,
                     const QString &acceptLabel, std::function<void()> onAccept);

    QString m_themeName;
    std::function<bool()> m_detectClaude;
    QWidget *m_claudeSection = nullptr;
    QWidget *m_installOffer = nullptr;
    std::vector<std::function<void()>> m_refreshRows;
    bool m_detected = false;
};

namespace welcome {

// Calls `show` and sets `ui.welcome_shown` when the key is false or absent;
// does nothing when it is true. MainWindow calls it from its first show.
void maybeAutoShow(Config &cfg, const std::function<void()> &show);

}  // namespace welcome
