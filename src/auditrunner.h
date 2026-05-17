// ANTS-1351 — server-side audit runner. Wraps the existing
// AuditEngine pure-function pipeline (ANTS-1119) + QProcess
// invocation of N external tools (cppcheck, clazy, ruff, bandit,
// semgrep, gitleaks, trivy, shellcheck, mypy) behind a single MCP
// verb. Returns a structured envelope + SARIF file path instead of
// shipping N raw tool outputs through parent context.
//
// Threading model (§ 2.5 of spec): runAudit blocks the caller. The
// MCP dispatcher routes this onto the dedicated m_auditPool worker
// (INV-9) so the GUI thread stays responsive; the worker hosts a
// local QEventLoop that multiplexes the per-tool QProcess signals.
//
// See docs/specs/ANTS-1351.md.

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace AuditRunner {

struct ToolSkip {
    QString tool;
    QString reason;
};

struct RunRequest {
    QString     projectRoot;             // canonical absolute (validated)
    QStringList tools;                   // empty == auto-detect
    QString     scope;                   // "auto" | "files" | "since-tag:<t>" | "branch-diff"
    int         capPerToolSeconds = 30;  // clamped [5, 60]; out-of-range → bad_args
    QString     suppressionsMode;        // "auto" | "none" | "path:<file>"
    QStringList formats;                 // {"sarif"} default; {"sarif","html"} for opt-in
    int         topFindingsCount = 0;    // [0, 100]; out-of-range → bad_args
};

struct ToolResult {
    QString    tool;
    QString    status;            // "ok" | "timed_out" | "not_runnable" | "crashed"
    qint64     elapsedMs = 0;
    int        rawCount = 0;
    int        afterFilterCount = 0;
    QJsonArray samples;           // each message ≤ 256 B
};

struct RunResult {
    bool                       ok = true;
    QString                    error;       // non-empty when ok==false
    QString                    code;        // refusal code on failure
    QHash<QString, ToolResult> byTool;
    int                        totalRaw = 0;
    int                        totalActionable = 0;
    int                        noiseRatePct = 0;
    QString                    sarifPath;
    QString                    htmlPath;
    QVector<ToolSkip>          toolsSkipped;
    qint64                     elapsedTotalMs = 0;
    bool                       samplesTruncated = false;
    QJsonArray                 topFindings;  // present iff topFindingsCount > 0
};

// Aggregate cap = min(tools.count * capPerToolSeconds * 1.5, 240 s).
// SIGTERM at per-tool cap; SIGKILL 2 s later. Stderr excerpts capped
// at 256 B and emitted in samples[0].message on crash (INV-4).
RunResult runAudit(const RunRequest &req);

// Pure helpers exposed for the engine test fixtures + the
// envelope-cap cascade (INV-13). Internal use only.
namespace internal {

// Envelope size measurement — implementer uses
// QJsonDocument(env).toJson(Compact).size() inline. Helper here
// for tests so they can replicate the cascade.
qint64 measureEnvelopeBytes(const QJsonArray &samples,
                            const QHash<QString, ToolResult> &byTool);

// Apply the bottom-up sample trim cascade: 10 → 5 → 3. Returns the
// trimmed samples and sets samplesTruncated when any trim fired.
void trimSamplesCascade(QHash<QString, ToolResult> &byTool,
                        bool &samplesTruncated);

// Cap message text to 256 B (UTF-8); appends "…" when truncated.
QString capMessage(const QString &msg);

}  // namespace internal

}  // namespace AuditRunner
