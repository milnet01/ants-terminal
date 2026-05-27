// ANTS-1897 — MCP discoverability via SessionStart hook installer.
// Spec: docs/specs/ANTS-1897.md.
//
// Bundled orientation script + Claude Code `~/.claude/settings.json`
// SessionStart hook entry. The hook prints a short MCP cheat-sheet at
// every Claude Code session start so a fresh session reaches for the
// Ants MCP variants (changelog_log, file_outline, find_definition, ...)
// instead of the always-loaded Edit/Write/Bash built-ins.

#pragma once

#include <QString>

namespace ants {
namespace mcp_orientation {

// Result of an install/uninstall operation. `ok=true` means the
// requested state was achieved (script + settings entry present, OR
// both absent if disabled). `warning` is non-empty when a non-fatal
// issue was logged (e.g. user-owned script preserved per INV-2,
// settings.json parse failure per INV-13).
struct Result {
    bool ok = false;
    QString warning;     // non-fatal note for the caller / status chip
    QString scriptPath;  // resolved install path for diagnostics
};

// The orientation script template. Carries one `%1` placeholder for
// `ANTS_VERSION` in the marker line; substitute via QString::arg.
// INV-12 byte-equality target.
//
// Placeholder contract per INV-12: exactly one `%1`, no other `%N`
// sequences. Any literal `%` in the body must be doubled `%%` so
// QString::arg does not consume it.
QString orientationScriptTemplate();

// Absolute path where the script is installed: typically
// $HOME/.config/ants-terminal/hooks/mcp-orientation.sh. Resolved from
// QStandardPaths::AppConfigLocation so the install path + the
// settings.json `command` string always agree (matters under XDG
// overrides + flatpak).
QString scriptInstallPath();

// Absolute path to the user's Claude Code settings file
// (~/.claude/settings.json). Honours $HOME.
QString claudeSettingsPath();

// The marker substring that identifies the Ants-owned SessionStart
// hook entry inside `~/.claude/settings.json`. Substring match on the
// `command` string per INV-3.
QString settingsMarkerSubstring();

// Install: write the script + merge the settings.json hook entry.
// Idempotent on the marker prefix (INV-1) and on the marker substring
// (INV-3); preserves all unrelated settings.json content (INV-4); does
// not overwrite a user-owned script (INV-2); does not overwrite a
// settings.json that fails to parse (INV-13).
Result install();

// Uninstall: remove the settings.json hook entry (the script file is
// left in place — it's harmless without the hook entry pointing at it,
// and removing it would also remove any user customisation).
Result uninstall();

// Test seam: install against a caller-supplied home directory instead
// of resolving $HOME. Used by tests/features/mcp_orientation_install/
// which sets up a QTemporaryDir.
Result installAt(const QString &homeDir);
Result uninstallAt(const QString &homeDir);

}  // namespace mcp_orientation
}  // namespace ants
