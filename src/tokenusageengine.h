// ANTS-1284 — in-process MCP token-usage tracker. Counts per-tool
// dispatch byte sizes and reports an est_tokens_saved delta against
// per-tool static baselines. See docs/specs/ANTS-1284.md.
//
// Qt6::Core only. Lives in ants_mcpcore_lib (ANTS-4932) alongside
// verifyengine, plantemplateengine, indiereviewengine, debtsweepengine,
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
    // ANTS-5311 — the raw counters, for ants-mcpd's snapshot writer.
    const QHash<QString, ToolCounter> &counters() const { return m_counters; }
    // ANTS-5104 — buildReport(false).totalSaved without building the report.
    qint64 totalSaved() const;

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

// ANTS-3579 — chars-per-token divisor, promoted from the tokenusageengine.cpp
// anonymous namespace to a public constexpr so the per-project display path
// (claudestatuswidgets.cpp) and its tests reference it by symbol rather than a
// bare `/ 4` literal.
constexpr qint64 kCharsPerToken = 4;  // ~Anthropic BPE order-of-magnitude

// Add `add` to monthly[monthKey] (values are JSON numbers, exact-integer to
// 2^53), then retain only the `keepMonths` lexicographically-greatest keys
// ("YYYY-MM" collates chronologically). Returns the updated, pruned map.
QJsonObject foldMonthlyBucket(QJsonObject monthly, const QString &monthKey,
                              qint64 add, int keepMonths);

// ANTS-3579 — per-project persistence helpers for the tokens-saved pill's
// `claude.tokens_saved_by_project` map. See docs/specs/ANTS-3579.md § 2.5.
//
// Fold ONE project's `addTokens` into byProject[root]: lifetime += addTokens,
// monthly via foldMonthlyBucket(...,keepMonths), stamp `since` = nowIso.left(10)
// (date portion) if absent, set `updated` = nowIso. Does NOT evict roots — that
// is pruneProjectBuckets, a separate single pass, so eviction is independent of
// the order roots are folded (INV-5b, M-2).
QJsonObject foldProjectBucket(QJsonObject byProject, const QString &root,
                              qint64 addTokens, const QString &monthKey,
                              const QString &nowIso, int keepMonths);

// Retain only the `keepProjects` roots with the newest `updated` datetime; ties
// broken by root string (larger evicted first) so the result is a pure function
// of the whole map. Call ONCE after the fold loop.
QJsonObject pruneProjectBuckets(QJsonObject byProject, int keepProjects);

// Sum of buckets whose key starts with `yearPrefix` (e.g. "2026") — the
// year-to-date total over the retained monthly map.
qint64 sumYear(const QJsonObject &monthly, const QString &yearPrefix);

// "820" / "1K" / "12.4K" / "1.2M" / "1B" — one decimal, trailing ".0"
// trimmed. UI-agnostic so the chip and any other surface share one format.
QString humanizeCount(qint64 n);

// ---- ANTS-5311 — ants-mcpd usage snapshots --------------------------------
// Each ants-mcpd writes its own counters to a snapshot file; the terminal reads
// every snapshot for display and folds one into config once its writer exits.
// Contract: docs/specs/ANTS-5311-mcpd-usage-snapshots.md.

// The floor-at-0 saving of one counter set, in tokens — Tracker::totalSaved()'s
// arithmetic, callable on a snapshot's counters alone.
qint64 totalSavedOf(const QHash<QString, ToolCounter> &counters);

// § 2.6 — what one snapshot may contribute. 64 matches the writer's
// ClaudeIntegration::kMaxTokenProjects, which is private.
constexpr int kMaxPeerProjects = 64;
constexpr int kMaxPeerTools    = 512;

// GenericDataLocation + "/ants-terminal/mcpd-usage".
QString peerSnapshotDir();

QJsonObject snapshotToJson(const QHash<QString, ToolCounter> &tools,
                           const QHash<QString, qint64> &savedBytesByProject,
                           qint64 pid, qint64 startedMs, qint64 updatedMs);
// The inverse, with § 2.6's checks. False and `why` set on any rejection.
bool snapshotFromJson(const QJsonObject &o, QHash<QString, ToolCounter> *tools,
                      QHash<QString, qint64> *savedBytesByProject, QString *why);
// Field-wise sum; min/max combine. Used by the writer's accumulator.
void addCounters(QHash<QString, ToolCounter> &into,
                 const QHash<QString, ToolCounter> &from);

// The peer total. Every figure is a sum of per-snapshot figures, each derived
// from that snapshot alone, so removing one snapshot removes exactly its own
// addends (§ 2.3, INV-4).
struct PeerUsage {
    qint64 savedTokens = 0;                       // Σ each snapshot's totalSavedOf()
    QHash<QString, qint64> savedTokensByProject;  // Σ each snapshot's bytes / kCharsPerToken
    qint64 calls = 0;                             // Σ n_calls
    qint64 failedCalls = 0;                       // Σ failed_calls
    int sessions = 0;                             // snapshots summed
    QStringList stems;                            // their stems
    QStringList skipped;                          // "<file>: <reason>"
};
// Read every snapshot in `dir`. With `claimDead`, each snapshot whose writer
// has exited is ALSO summed into `dead`, its lock held in `heldLocks` until
// releaseClaimed().
PeerUsage readPeerSnapshots(const QString &dir, bool claimDead,
                            PeerUsage *dead, QList<int> *heldLocks);
// Unlinks each claimed snapshot's files and closes the held locks.
void releaseClaimed(const QString &dir, const PeerUsage &dead,
                    const QList<int> &heldLocks);
// Folds `dead` into the three config values through foldMonthlyBucket /
// foldProjectBucket / pruneProjectBuckets. Pure. True when a value changed.
bool foldPeerUsage(const PeerUsage &dead, QJsonObject &monthly, qint64 &lifetime,
                   QJsonObject &byProject, const QString &month,
                   const QString &nowIso);

}  // namespace TokenUsageEngine
