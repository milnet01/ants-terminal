// ANTS-5736 — the Ants verbs a project's recent Claude Code sessions loaded
// through ToolSearch, offered as one ready-made load line in the MCP
// `initialize` reply's `instructions`, so a session loads them in one call.
//
// Read from the project's own transcripts: a verb that needed a ToolSearch
// `select:` is exactly a deferred one this project used, so the always-loaded
// verbs never appear and no list of them is kept here. Qt6::Core only.

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace mcp {

// Claude Code's transcript folder for a session started in `projectDir`:
// <config>/projects/<projectDir with every non-alphanumeric character as '-'>,
// where <config> is $CLAUDE_CONFIG_DIR or ~/.claude.
QString transcriptDirFor(const QString &projectDir);

// The tool names the newest `maxFiles` transcripts in `dir` loaded with a
// ToolSearch `select:` query, keeping only names ending in `__<verb>` for a
// verb in `known`. Ranked by how many transcripts loaded each, then by name;
// at most `maxNames`. Empty when the folder is missing or nothing matched.
QStringList recentlyLoadedTools(const QString &dir, const QSet<QString> &known,
                                int maxFiles = 5, int maxNames = 8);

// The `instructions` text for `tools`, or empty when there are none.
QString loadHintText(const QStringList &tools);

}  // namespace mcp
