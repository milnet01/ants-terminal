// ANTS-1894 — Near-miss telemetry sibling to the firing ledger.
//
// Records gate-evaluated-but-blocked auto-switch decisions (every tick where
// ModelAutoSwitch::decide returned act=false with a non-empty blockedBy list).
// Diagnostic surface for the question "why doesn't the auto-switcher fire in
// this project?" — see docs/specs/ANTS-1894.md.
//
// Lives in ants_core_lib (alongside modelswitchledger) so the read-only
// model_switch_stats MCP verb (dispatched from RemoteControl, also in
// ants_core_lib) can reach it without a cross-lib call. The producer
// (ClaudeStatusBarController::maybeEmitNearMiss in ants_claude_lib) calls
// across libs the same way it already does for ModelSwitchLedger::appendRecord.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace ModelNearMissLedger {

constexpr qint64 kMaxLedgerBytes = 256 * 1024;             // 256 KiB; mirrors firing ledger INV-10
constexpr qint64 kStatsWindowMs  = 24 * 60 * 60 * 1000;    // 24 h cutoff used by statsSlim / statsFull

// Record schema — § 2.3. JSON keys are snake_case, matching the firing
// ledger's style. Optional fields default via the struct defaults; fromJson
// reads missing fields as the struct defaults (INV-13).
struct Record {
    QString     ts;                       // ISO-8601 UTC, second precision via ModelSwitchLedger::nowIso8601()
    QString     sessionId;                // ClaudeIntegration::lastHookSessionId() or ""
    QString     project;                  // focused tab shellCwd()
    QString     currentTier;              // "haiku" | "sonnet" | "opus"
    QString     recommendedTier;          // clamped target alias
    QStringList blockedBy;                // ordered subset of the taxonomy tokens (INV-9; 9 since ANTS-1917/1959)
    bool        composerEmpty       = true;
    QString     focusedState;             // lowercase snake_case ClaudeState
    int         ticksTargetStable   = 0;
    qint64      dwellMs             = 0;
    qint64      msSinceLastOverride = -1; // sentinel matches Gate.msSinceLastOverride
    int         epoch              = 0;  // ANTS-1954 — behaviour epoch (mirrors
                                          // firing ledger ANTS-1941); 0 = pre-epoch
                                          // record (written before the field existed)
};

// --- I/O paths + helpers ---------------------------------------------------

QString defaultLedgerPath();           // ~/.cache/ants-terminal/model-switch-nearmiss.jsonl

// --- JSON --------------------------------------------------------------------

QJsonObject toJson(const Record &r);
Record      fromJson(const QJsonObject &o);   // missing fields → struct defaults (INV-13)

// --- On-disk I/O (atomic via QSaveFile, mode 0600) ---------------------------

// appendRecord: append one record, evict whole oldest lines until ≤ capBytes.
// Unlike the firing ledger, NO pending-pinning (near-miss records have no
// outcome backfill). The newest line is never dropped (INV-8).
bool appendRecord(const QString &path, const Record &rec,
                  qint64 capBytes = kMaxLedgerBytes);
QList<Record> readRecords(const QString &path);

// evictToCap: drop whole oldest lines until total ≤ capBytes. Newest line
// always preserved. No mid-line truncation. Pure — exposed for testing.
QList<QByteArray> evictToCap(QList<QByteArray> lines, qint64 capBytes);

// --- Aggregation (consumed by model_switch_stats) ----------------------------

// Slim block folded into the default model_switch_stats envelope. Per-record
// blockers contribute one count per distinct token; dominantBlocker is the
// max-count token (ties broken by INV-9 taxonomy order). Empty window →
// dominantBlocker == "".
struct StatsSlim {
    int     total24h = 0;
    QString dominantBlocker;
    // ANTS-1960 — idle_end_of_session-only records excluded from total24h and
    // dominantBlocker (blocking at session-end is correct behaviour, not a
    // missed opportunity). Exposed as a separate count in the envelope.
    int     idleEndSuppressed24h = 0;
};
// ANTS-1954 — minEpoch mirrors the firing-ledger ANTS-1941 filter: records
// with epoch < minEpoch are excluded from both window_24h and all_time, so
// dominant_blocker resets cleanly after a rebuild rather than after 24h.
// Pass ModelSwitchLedger::kSwitcherEpoch at live dispatch sites; 0 = no filter.
StatsSlim statsSlim(const QList<Record> &recs,
                    const QString       &projectScope,   // "" for global
                    qint64               nowMs,
                    int                  minEpoch = 0);

// Full breakdown — returned when model_switch_stats is called with
// mode:"near_misses". window_24h is filtered by nowMs; all_time has no
// window filter. distinct_signatures counts distinct post-sort blocked_by
// arrays. Headline gives the human summary.
// ANTS-1954 — minEpoch: excludes pre-epoch records; surfaces min_epoch +
// excluded_pre_epoch_count in the envelope when > 0.
QJsonObject statsFull(const QList<Record> &recs,
                      const QString       &projectScope,
                      qint64               nowMs,
                      int                  minEpoch = 0);

}  // namespace ModelNearMissLedger
