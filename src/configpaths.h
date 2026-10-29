#pragma once

// ConfigPaths — the ~/.claude/... and ~/.config/ants-terminal/hooks/...
// locations this project reads and writes, each named once.
//
// Not config.json: Config::configPath() finds that through
// QStandardPaths, so it follows XDG_CONFIG_HOME. antsConfigHome() below
// is always ~/.config/ants-terminal, so with XDG_CONFIG_HOME set the
// hooks folder and config.json are in different places (ANTS-6291).
//
// Paths are returned as absolute QStrings. Callers are responsible for
// ensuring the directory exists (QDir::mkpath) before writing.

#include <QDir>
#include <QStandardPaths>
#include <QString>

namespace ConfigPaths {

// ~/.claude — the Claude Code configuration root (owned by Claude Code,
// not this project; we only read + write specific files under it).
inline QString claudeHome() {
    return QDir::homePath() + QStringLiteral("/.claude");
}

// ~/.claude/projects — per-project transcript subtree.
inline QString claudeProjectsDir() {
    return claudeHome() + QStringLiteral("/projects");
}

// ~/.claude/sessions — active-session metadata (one .json per session).
inline QString claudeSessionsDir() {
    return claudeHome() + QStringLiteral("/sessions");
}

// ~/.claude/settings.json — global Claude Code settings (hooks,
// permissions, model, etc.). We merge into this file, never overwrite.
inline QString claudeSettingsJson() {
    return claudeHome() + QStringLiteral("/settings.json");
}

// ~/.claude/projects/<encoded>/memory/MEMORY.md — auto-memory index.
inline QString claudeProjectMemory(const QString &projectEncoded) {
    return claudeProjectsDir() + QLatin1Char('/') + projectEncoded
         + QStringLiteral("/memory/MEMORY.md");
}

// ~/.config/ants-terminal — this project's own config root.
inline QString antsConfigHome() {
    return QDir::homePath() + QStringLiteral("/.config/ants-terminal");
}

// ~/.config/ants-terminal/hooks — directory where we write helper shell
// scripts (e.g. claude-forward.sh for Claude Code hook forwarding).
inline QString antsHooksDir() {
    return antsConfigHome() + QStringLiteral("/hooks");
}

// ~/.config/ants-terminal/hooks/claude-forward.sh — forwarder script
// invoked by Claude Code hook entries in claudeSettingsJson().
inline QString antsClaudeForwardScript() {
    return antsHooksDir() + QStringLiteral("/claude-forward.sh");
}

// ~/.config/ants-terminal/hooks/claude-git-context.sh — UserPromptSubmit
// hook script that prints a <git-context> block so Claude Code doesn't
// need to run `git status` via the Bash tool. See
// tests/features/claude_git_context_hook/spec.md.
inline QString antsClaudeGitContextScript() {
    return antsHooksDir() + QStringLiteral("/claude-git-context.sh");
}

// ANTS-5236 — <RuntimeLocation>/ants-terminal, the private directory the
// Claude hook and MCP sockets bind in. Empty when Qt has no runtime location.
// RuntimeLocation is $XDG_RUNTIME_DIR, or Qt's own 0700 fallback under
// tempPath() when that is unset or not the user's at 0700.
inline QString antsRuntimeDir() {
    const QString runtime =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) return {};
    return runtime + QStringLiteral("/ants-terminal");
}

}  // namespace ConfigPaths
