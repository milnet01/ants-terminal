// ANTS-1735 — Effectiveness ledger for the autonomous model switcher.
//
// Every auto-switch appends one Record; a later tick fills its Outcome once the
// following turns have run. The ledger is the *trust signal* that replaces the
// (unused) chip-click of Shape A: measured avoided-Opus vs regret/under-route.
// See docs/specs/ANTS-1735.md §2.5 + INV-10..INV-12.
//
// Pure Qt6 Core/JSON unit, NO ModelRecommender / claude_lib dependency — it
// lives in ants_core_lib so the read-only `model_switch_stats` MCP verb
// (dispatched from mainwindow.cpp / ants_chrome_lib) can reach it. Tiers are
// stored as the lowercase aliases ("haiku"/"sonnet"/"opus"); ranking is local.
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace ModelSwitchLedger {

// INV-10 — drop-oldest byte cap. INV-11 — author-correlation window.
constexpr qint64 kMaxLedgerBytes     = 256 * 1024;   // 256 KiB
constexpr qint64 kAuthorWindowMs     = 10'000;       // 10 s
constexpr int    kOutcomeWindowTurns = 5;            // "within 5 turns"

struct Outcome {
    int  turnsOnToTier            = 0;
    bool userOverrideWithin5      = false;
    bool correctionSignalWithin5  = false;
    bool underRouteSignalWithin5  = false;
    bool pending                  = true;   // outcome not yet filled in
};

struct Record {
    QString ts;            // ISO-8601 Z, e.g. "2026-05-25T14:02:11Z"
    QString sessionId;
    QString project;       // project root path
    QString fromTier;      // "haiku"|"sonnet"|"opus"
    QString toTier;
    QString scoreReason;   // ModelRecommender rec.reason at switch time
    QString trigger;       // "auto"
    Outcome outcome;
};

// --- JSON (field names match the spec §2.5 record verbatim) ---
QJsonObject toJson(const Record &rec);
Record      fromJson(const QJsonObject &obj);

// --- On-disk I/O (atomic via QSaveFile, mode 0600 via setOwnerOnlyPerms) ---
// appendRecord: append one record, then evict oldest non-pending lines until the
// file is within capBytes (pending records are pinned; newest kept) — INV-10.
bool appendRecord(const QString &path, const Record &rec,
                  qint64 capBytes = kMaxLedgerBytes);
QList<Record> readRecords(const QString &path);
// writeRecords: replace the whole ledger (read-modify-write for outcome
// fill-in), applying the same eviction.
bool writeRecords(const QString &path, const QList<Record> &recs,
                  qint64 capBytes = kMaxLedgerBytes);

// evictToCap: drop whole oldest *non-pending* lines until total ≤ capBytes.
// Pending records are never dropped; the newest line is never dropped; lines are
// removed whole, never truncated mid-line (INV-10). Pure — exposed for testing.
QList<QByteArray> evictToCap(QList<QByteArray> lines, qint64 capBytes);

// --- Outcome detection (pure; the controller feeds live transcript data) ---
struct ModelEvent { QString tier; qint64 tsMs = 0; };   // a transcript `/model X`
struct AutoSwitch { QString toTier; qint64 tsMs = 0; }; // an auto ledger record

// INV-11 — true iff any in-window transcript /model event is NOT auto-authored
// (no auto record matches by tier AND |Δts| ≤ authorWindowMs).
bool detectUserOverride(const QList<ModelEvent> &windowModelEvents,
                        const QList<AutoSwitch> &autoRecords,
                        qint64 authorWindowMs = kAuthorWindowMs);

// INV-12 — under-route on a downgrade: a higher tier re-recommended within the
// window. Zero following turns → Pending (never counted as not-under-routed).
enum class UnderRoute { Pending, No, Yes };
UnderRoute detectUnderRoute(const QString &toTier,
                            const QStringList &subsequentRecommendedTiers);

// MEDIUM-2 — soft correction signal over the first post-downgrade user turn.
// Linear regex (no ReDoS); false-fires on prose by design — never ground truth.
bool detectCorrection(const QString &firstUserTurnText);

// --- INV-13 — read-only aggregation for the model_switch_stats MCP verb ---
// statsEnvelope is pure over a record list; statsForProject reads the (global)
// ledger and filters to one project root before aggregating. Both are
// read-only. An absent ledger yields {ok:true, switches:0, …}. The headline is
// reported as an avoided/regret ratio; pending records are counted separately
// from outcome stats (never silently counted as success).
QJsonObject statsEnvelope(const QList<Record> &recs);
QJsonObject statsForProject(const QString &ledgerPath, const QString &projectRoot);

// haiku=0, sonnet=1, opus=2; unknown alias → -1.
int tierRank(const QString &alias);

// ~/.cache/ants-terminal/model-switch-ledger.jsonl
QString defaultLedgerPath();

// ISO-8601 Z stamp / parse helpers for the controller's ts handling.
QString nowIso8601();
qint64  parseIso8601Ms(const QString &ts);

}  // namespace ModelSwitchLedger
