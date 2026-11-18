#pragma once

// session_cost (ANTS-5698) — the seam below the handler: a session's context
// rent, read from its transcript. Contract: tests/features/session_cost/spec.md.
// Its own translation unit so a test links it without RemoteControl
// (docs/standards/mcp-tools.md § Tests). The clock is a parameter so a test
// can pin it.

#include <QJsonObject>
#include <QSet>
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

// The transcript folder for `cwd`, else for its nearest ancestor that has one,
// trying `projectsDirs` in order at each level; empty when none has one.
// session_cost and run_trace both resolve through it (ANTS-6271).
QString folderFor(const QStringList &projectsDirs, const QString &cwd);

// ANTS-6272 — which transcript lines carry a message's usage, for
// session_cost and run_trace alike. The harness writes one assistant line per
// content block, each with the whole message's usage, so a message counts
// once by its id; a line with no id or no usage carries none. A helper agent's
// line copied into a main transcript (isSidechain) is not the session's,
// unless `sidechainIsOwn`: the transcript is the agent's own (ANTS-6189).
enum class UsageLine { None, Duplicate, Counted };
UsageLine messageUsage(const QJsonObject &entry, bool sidechainIsOwn,
                       QSet<QString> &seen, QJsonObject *usage);
// True for a helper agent's own transcript, <session>/subagents/agent-*.jsonl.
bool isAgentTranscript(const QString &transcriptPath);

// Where the harness keeps transcripts: $CLAUDE_CONFIG_DIR/projects when that
// is set, then ~/.claude/projects. Read by the handlers, not by run(), so a
// test passes its own list.
QStringList projectsDirs();

// ANTS-6826 — the model price file, share/ants-terminal/model-prices.json.
// `error` is empty when it loaded; it names the file when it did not.
struct Prices {
    QString path;
    QString read;            // the date the prices were read from the pricing page
    QJsonObject models;      // model id -> prices, USD per million tokens
    QJsonObject geo;         // inference_geo -> multiplier
    QString error;
};
Prices loadPrices(const QString &path);
// The installed price file: beside the binary's share/ directory, then
// /usr/local/share and /usr/share. Read by the handler, not by run(), so a
// test passes its own; the handler loads it on every call (hot reload).
QString pricesFile();

// The whole report for one transcript (INV-1 to INV-3, INV-10 to INV-15),
// with its estimated cost at `prices` (INV-17, INV-18). `top` caps each list.
QJsonObject analyse(const QString &transcriptPath, int top, const Prices &prices = {});

// Resolves the transcript from cwd and args (INV-4, INV-5), then analyse();
// `include_agents` adds the helper agents (INV-16, INV-19).
// `projectsDirs` are tried in order; `nowMs` dates "active".
QJsonObject run(const QStringList &projectsDirs, const QString &cwd,
                const QJsonObject &args, qint64 nowMs, const Prices &prices = {});

}  // namespace SessionCost
