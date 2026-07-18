// ANTS-1284 — in-process MCP token-usage tracker. Counts per-tool
// dispatch byte sizes and reports an est_tokens_saved delta against
// per-tool static baselines. See docs/specs/ANTS-1284.md.
//
// Qt6::Core only. Lives in ants_core_lib alongside verifyengine,
// plantemplateengine, indiereviewengine, debtsweepengine,
// roadmapfoldin.

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace TokenUsageEngine {

struct ToolCounter {
    int    nCalls         = 0;
    qint64 bytesIn        = 0;
    qint64 bytesOut       = 0;
    // ANTS-1355 — v2 accumulators.
    qint64 wrapBytes      = 0;
    qint64 durationUsMin  = 0;  // sentinel "unset" → nCalls==0
    qint64 durationUsMax  = 0;
    qint64 durationUsSum  = 0;  // private; not surfaced (INV-9)
    // ANTS-1432 — failed-call accumulators. Mutually exclusive with
    // the success-path fields above: a single recordCall touches one
    // side or the other, never both.
    qint64 failedCalls    = 0;
    qint64 failedBytesIn  = 0;
    qint64 failedBytesOut = 0;
};

struct ToolReport {
    QString tool;
    int     nCalls         = 0;
    qint64  bytesIn        = 0;
    qint64  bytesOut       = 0;
    // ANTS-1355 — v2 fields.
    qint64  wrapBytes      = 0;
    qint64  durationUsMin  = 0;
    qint64  durationUsMax  = 0;
    qint64  durationUsMean = 0;  // floor(sum / n_calls)
    qint64  estTokensSaved = 0;
    // ANTS-1432 — failed-call fields. Surface waste-on-failure so
    // callers can compute net-token-impact.
    qint64  failedCalls    = 0;
    qint64  failedBytesIn  = 0;
    qint64  failedBytesOut = 0;
};

struct Snapshot {
    qint64           sinceUnixMs    = 0;
    QList<ToolReport> calls;        // sorted by estTokensSaved desc, then tool asc
    qint64           totalSaved     = 0;
    qint64           totalWrapBytes = 0;  // ANTS-1355 — sum across ALL tools
    // ANTS-1432 — Σ(failedBytesIn + failedBytesOut) across ALL tools
    // (includes tools filtered out of `calls[]` by include_zero:false).
    qint64           totalFailedBytes = 0;
    int              toolsCalled    = 0;
};

class Tracker {
public:
    Tracker();

    // ANTS-1355 — v2 signature adds wrapBytes + durationUs. Callers
    // that don't have those values (tests) pass 0.
    // ANTS-1432 — v3 signature adds `success`. Default `true` keeps
    // existing test-helper call sites untouched. Failed calls (false)
    // accumulate into the failed-* fields instead of the success
    // accumulators — see header doc on ToolCounter.
    void recordCall(const QString &toolName,
                    qint64 bytesIn,
                    qint64 bytesOut,
                    qint64 wrapBytes = 0,
                    qint64 durationUs = 0,
                    bool   success    = true);
    void reset();

    Snapshot buildReport(bool includeZero) const;

    static qint64 baselineFor(const QString &toolName);

    qint64 sinceUnixMs() const { return m_sinceUnixMs; }

private:
    QHash<QString, ToolCounter> m_counters;
    qint64                       m_sinceUnixMs = 0;
};

// ANTS-3572 — pure persistence helpers for the tokens-saved aggregate
// (status-bar chip + monthly / YTD / all-time). Kept here (Qt6::Core, no
// Widgets) so they are unit-testable without a MainWindow; the fold/write
// side that consumes them lives on MainWindow. See docs/specs/ANTS-3572.md.

// Add `add` to monthly[monthKey] (values are JSON numbers, exact-integer to
// 2^53), then retain only the `keepMonths` lexicographically-greatest keys
// ("YYYY-MM" collates chronologically). Returns the updated, pruned map.
QJsonObject foldMonthlyBucket(QJsonObject monthly, const QString &monthKey,
                              qint64 add, int keepMonths);

// Sum of buckets whose key starts with `yearPrefix` (e.g. "2026") — the
// year-to-date total over the retained monthly map.
qint64 sumYear(const QJsonObject &monthly, const QString &yearPrefix);

// "820" / "1K" / "12.4K" / "1.2M" / "1B" — one decimal, trailing ".0"
// trimmed. UI-agnostic so the chip and any other surface share one format.
QString humanizeCount(qint64 n);

}  // namespace TokenUsageEngine
