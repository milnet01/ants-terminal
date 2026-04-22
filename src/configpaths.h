#pragma once

// ConfigPaths — single source of truth for filesystem locations this
// project reads/writes under the user's home directory. Introduced in
// 0.7.9 to consolidate the ~/.config/ants-terminal/... and ~/.claude/...
// strings that were duplicated across 4 files (claudeintegration.cpp,
// settingsdialog.cpp, claudeallowlist.cpp, mainwindow.cpp).
//
// A future XDG-respect pass (honoring XDG_CONFIG_HOME / XDG_DATA_HOME
// instead of hard-coding ~/.config) will have one call site to update.
//
// Paths are returned as absolute QStrings. Callers are responsible for
// ensuring the directory exists (QDir::mkpath) before writing.

#include <QDir>
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

}  // namespace ConfigPaths
