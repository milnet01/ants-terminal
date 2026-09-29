// ANTS-5558 — the one-click setup actions the welcome dialog and Settings
// share: the Claude Code hooks, registering ants-mcpd with Claude Code, the
// optional CLAUDE.md note, and shell integration. Free functions with no
// widget or dialog in them, so the two surfaces cannot drift apart.
// Contract: docs/specs/ANTS-5558-welcome-dialog.md § 2.3 to § 2.6.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace ants::claude_setup {

enum class State : std::uint8_t { Installed, Partial, Missing, Unavailable };

struct Status {
    State   state = State::Missing;
    QString detail;
};

struct Outcome {
    bool    ok = false;
    QString message;
};

// Claude Code is present when `claude` is on PATH or ~/.claude exists.
bool claudeCodeDetected();

// The status-bar hooks (formerly SettingsDialog::installClaudeHooks).
Status  statusHooksStatus();
Outcome installStatusHooks();

// The git-context UserPromptSubmit hook.
Status  gitContextStatus();
Outcome installGitContextHook();

// Registering ants-mcpd with Claude Code at user scope.
// The command to register: {$APPIMAGE, "--mcpd"} when `appImage` is set,
// else {the ants-mcpd beside `appDir`, else on PATH}. Empty when none.
QStringList mcpLaunchCommand(const QString &appImage, const QString &appDir);
// The `claude` argv lists to run, in order: a `mcp remove --scope user ants`
// first when a user registration exists, then `mcp add --scope user ants --`.
QList<QStringList> mcpRegistrationCommands(const QStringList &launch,
                                           bool alreadyRegistered);
Status  mcpStatus();
Outcome registerMcp();

// The optional note in ~/.claude/CLAUDE.md, between its begin/end markers.
QString defaultClaudeMdPath();
QString claudeMdNoteText();
Status  claudeMdNoteStatus(const QString &path);
Outcome installClaudeMdNote(const QString &path);
Outcome removeClaudeMdNote(const QString &path);

// Shell integration (OSC 133). `scriptDirs` is the search list for
// ants-osc133.<shell>, defaulting to defaultShellScriptDirs().
QStringList defaultShellScriptDirs();
QString     shellIntegrationPreview(const QStringList &scriptDirs);
Status      shellIntegrationStatus(const QStringList &scriptDirs);
Outcome     installShellIntegration(const QStringList &scriptDirs);

}  // namespace ants::claude_setup
