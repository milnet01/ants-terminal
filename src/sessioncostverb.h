#pragma once

// session_cost (ANTS-5698) — the seam below the handler: a session's context
// rent, read from its transcript. Contract: tests/features/session_cost/spec.md.
// Its own translation unit so a test links it without RemoteControl
// (docs/standards/mcp-tools.md § Tests). The clock is a parameter so a test
// can pin it.

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace SessionCost {

inline constexpr int kDefaultTop = 8;
inline constexpr int kMaxTop = 50;
// A transcript written within this window counts as an active session.
inline constexpr qint64 kActiveWindowMs = 10LL * 60 * 1000;

// The harness's folder name for a cwd: every character outside ASCII
// letters and digits becomes '-'.
QString folderNameFor(const QString &cwd);

// Where the harness keeps transcripts: $CLAUDE_CONFIG_DIR/projects when that
// is set, then ~/.claude/projects. Read by the handlers, not by run(), so a
// test passes its own list.
QStringList projectsDirs();

// The whole report for one transcript (INV-1 to INV-3). `top` caps each list.
QJsonObject analyse(const QString &transcriptPath, int top);

// Resolves the transcript from cwd and args (INV-4, INV-5), then analyse().
// `projectsDirs` are tried in order; `nowMs` dates "active".
QJsonObject run(const QStringList &projectsDirs, const QString &cwd,
                const QJsonObject &args, qint64 nowMs);

}  // namespace SessionCost
