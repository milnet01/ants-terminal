// ANTS-5558 — see claudesetup.h. Interface stubs: the behaviour lands with
// the implementation, after the conformance tests are proven red.

#include "claudesetup.h"

namespace ants::claude_setup {

bool claudeCodeDetected() { return false; }

Status  statusHooksStatus() { return {}; }
Outcome installStatusHooks() { return {}; }
Status  gitContextStatus() { return {}; }
Outcome installGitContextHook() { return {}; }

QStringList mcpLaunchCommand(const QString &, const QString &) { return {}; }
QList<QStringList> mcpRegistrationCommands(const QStringList &, bool) {
    return {};
}
Status  mcpStatus() { return {}; }
Outcome registerMcp() { return {}; }

QString defaultClaudeMdPath() { return {}; }
QString claudeMdNoteText() { return {}; }
Status  claudeMdNoteStatus(const QString &) { return {}; }
Outcome installClaudeMdNote(const QString &) { return {}; }
Outcome removeClaudeMdNote(const QString &) { return {}; }

QStringList defaultShellScriptDirs() { return {}; }
QString     shellIntegrationPreview(const QStringList &) { return {}; }
Status      shellIntegrationStatus(const QStringList &) { return {}; }
Outcome     installShellIntegration(const QStringList &) { return {}; }

}  // namespace ants::claude_setup
