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
constexpr qint64 kMaxLedgerBytes        = 256 * 1024;   // 256 KiB
constexpr qint64 kAuthorWindowMs        = 10'000;       // 10 s
constexpr int    kOutcomeWindowTurns    = 5;            // "within 5 turns"
constexpr qint64 kCleanEndQuietMs       = 10 * 60 * 1000; // ANTS-1891 — 10 min
constexpr int    kHeadlineFloorMeasured = 10;           // ANTS-1891 — headline floor
constexpr int    kDefaultStatsWindowDays = 30;          // ANTS-1936/1942 — recency window

// ANTS-1941 — behaviour epoch. Bump by 1 in any commit that changes a
// behaviour the trust signal measures (recommender scoring, the decide() gate,
// the actuator, or the outcome fill-in). Records written before this feature
// shipped carry no `epoch` field and read as 0 (pre-epoch). Epoch 1 == the
// first switch written *after ANTS-1941 ships* — a point that is ≥ every prior
// behaviour fix (ANTS-1916/1930/…), so a safe "fixed-from-here" lower bound.
// NEVER decrement; NEVER reuse a value (monotonicity is a review rule).
// ANTS-1957 — bumped 1 → 2: the regret signal now counts only a *corrective*
// (upward) override, not any manual /model, so pre-fix records (scored under the
// old "any override = regret" rule) are not comparable and are excluded by the
// epoch boundary. The trust signal recalibrates cleanly from the first switch
// written after this ships.
constexpr int    kSwitcherEpoch         = 2;            // ANTS-1941, ANTS-1957
static_assert(kSwitcherEpoch >= 1,
              "epoch is a positive behaviour-generation counter");

struct Outcome {
    int  turnsOnToTier            = 0;
    bool userOverrideWithin5      = false;
    bool correctionSignalWithin5  = false;
    bool underRouteSignalWithin5  = false;
    bool sessionCleanlyEndedOnNewTier = false;  // ANTS-1891 — positive signal
    // ANTS-1957 — direction-aware regret signal: true iff a non-auto-authored
    // /model within the window moved to a tier ABOVE this downgrade's toTier
    // (i.e. the user undid the downgrade). Distinct from userOverrideWithin5
    // (which flags ANY manual switch and still drives the ANTS-1890 cool-down +
    // clean-end conservatism). REGRET counts this narrow signal, not the broad
    // one — a lateral / further-down / same-tier manual switch is not evidence
    // the downgrade was wrong.
    bool overrideUndidDowngrade   = false;
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
    int     epoch = 0;     // ANTS-1941 — behaviour epoch; 0 = pre-epoch record
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

// INV-13 (ANTS-1957) — true iff any in-window non-auto-authored /model event
// moves to a tier strictly ABOVE `toTier` (the user undid the downgrade by
// moving back up). Lateral / same-tier / further-down overrides are NOT
// corrective and never count as regret against this switch. Direction-aware
// refinement of detectUserOverride that feeds the regret signal ONLY; the broad
// detectUserOverride still drives the cool-down + clean-end conservatism.
bool detectCorrectiveOverride(const QList<ModelEvent> &windowModelEvents,
                              const QList<AutoSwitch> &autoRecords,
                              const QString &toTier,
                              qint64 authorWindowMs = kAuthorWindowMs);

// INV-12 — under-route on a downgrade: a higher tier re-recommended within the
// window. Zero following turns → Pending (never counted as not-under-routed).
enum class UnderRoute { Pending, No, Yes };
UnderRoute detectUnderRoute(const QString &toTier,
                            const QStringList &subsequentRecommendedTiers);

// MEDIUM-2 — soft correction signal over the first post-downgrade user turn.
// Linear regex (no ReDoS); false-fires on prose by design — never ground truth.
bool detectCorrection(const QString &firstUserTurnText);

// modelId → alias mapping ("claude-haiku-…" → "haiku", "claude-opus-…" →
// "opus", everything else → "sonnet"; empty modelId → ""). Mirrors
// ModelRecommender::tierFromModelId without taking a claude_lib dependency
// (ants_core_lib must not link claude_lib). Lower-case alias matches
// `toTier` field encoding in the ledger.
QString aliasFromModelId(const QString &modelId);

// One assistant or user turn projected from the transcript, with just the
// fields the outcome fill-in helper needs. Caller (controller) parses the
// transcript and supplies these in chronological order.
struct TranscriptTurn {
    qint64  tsMs        = 0;     // assistant.timestamp parsed to ms
    bool    isAssistant = false;
    bool    isUser      = false;
    QString modelId;             // assistant: message.model
    QString userText;            // user: joined text content
};

struct OutcomeFillResult {
    Outcome outcome;
    bool    changed = false;     // outcome differs from inputBefore — caller flushes
};

// computeOutcome — fill in one ledger record's outcome from observed
// post-switch transcript turns + later auto-records + the latest
// recommender outputs. Pure.
//
// Settlement (`outcome.pending = false`) when ANY of:
//   - at least one subsequent auto-switch follows this record's ts (the dwell
//     has ended), OR
//   - at least `kOutcomeWindowTurns` assistant turns observed post-switch.
// Otherwise stays pending (controller leaves the record alone).
//
// turns_on_to_tier counts assistant turns whose modelId aliases to `toTier`,
// until the first divergent assistant turn OR a subsequent auto-switch.
//
// user_override / correction / under_route follow §2.5 + INV-11/12; correction
// and under_route only fire on a downgrade (tierRank(from) > tierRank(to)).
OutcomeFillResult computeOutcome(
    const QString &fromTier, const QString &toTier,
    qint64 switchTsMs,
    const QList<TranscriptTurn> &postSwitchTurns,
    const QList<AutoSwitch> &subsequentAutoSwitches,
    const QStringList &subsequentRecommendedTiers,
    const Outcome &inputBefore = {},
    qint64 nowMs = 0);   // ANTS-1891 — clock seam for the quiet-window
                         // settlement (defaulted to 0 = pre-1891 behaviour)

// --- INV-13 — read-only aggregation for the model_switch_stats MCP verb ---
// statsEnvelope is pure over a record list; statsForScope reads the (global)
// ledger and either filters to one project root or aggregates across all
// projects depending on `cfg.scope`. Both are read-only. An absent ledger
// yields {ok:true, switches:0, …}. Pending records are counted separately from
// outcome stats (never silently counted as success).
//
// ANTS-1889 — the envelope also carries the live switcher configuration
// (auto_model_switch_enabled / floor_tier / min_dwell_sec / scope) so a
// caller can distinguish "feature OFF" from "feature ON, no candidate
// turns yet" from "feature ON with measured outcomes". The headline reads
// "auto-switch OFF" when disabled, "auto-switch ON … (no switches yet)"
// when enabled with zero in-scope records, and the avoided/regret ratio
// otherwise.
struct StatsConfig {
    bool    switchEnabled = false;
    QString floorTier     = QStringLiteral("haiku");   // "haiku" | "sonnet"
    int     minDwellSec   = 90;
    QString scope         = QStringLiteral("project"); // "project" | "global"
    // ANTS-1909 — optional near-miss summary, surfaced in the headline when
    // the firings ledger is empty or below the headline floor. Zero / empty
    // disables the enrichment so existing callers see no change.
    int     nearMissTotal24h     = 0;
    QString nearMissDominantBlocker;
    // ANTS-1936 — recency window in days. Records older than this are excluded
    // from the aggregation to prevent stale pre-fix records from poisoning the
    // trust signal indefinitely. 0 = no window (all-time). ANTS-1942 — the
    // default and both live dispatch sites share kDefaultStatsWindowDays so they
    // can never silently diverge.
    int     windowDays           = kDefaultStatsWindowDays;
    // ANTS-1941 — minimum behaviour epoch. Records with epoch < minEpoch are
    // excluded from the aggregation (counted in excluded_pre_epoch_count). A
    // record with epoch >= minEpoch is in-scope (a newer behaviour generation is
    // trusted): the predicate is a strict `<`, NEVER `!=`. 0 = no epoch filter
    // (all-time / forensic path). Live dispatch sites set this to kSwitcherEpoch.
    int     minEpoch             = 0;
};
QJsonObject statsEnvelope(const QList<Record> &recs,
                          const StatsConfig &cfg = {});
// scope:"global" ignores projectRoot and aggregates the whole ledger;
// scope:"project" filters to projectRoot before aggregating.
QJsonObject statsForScope(const QString &ledgerPath,
                          const QString &projectRoot,
                          const StatsConfig &cfg);
// Back-compat thin wrapper — defaults to scope:"project" with a
// disabled switcher (no config surfaced). Prefer statsForScope at the
// MCP dispatch site so the live config triple lands in the envelope.
QJsonObject statsForProject(const QString &ledgerPath, const QString &projectRoot);

// haiku=0, sonnet=1, opus=2; unknown alias → -1.
int tierRank(const QString &alias);

// ~/.cache/ants-terminal/model-switch-ledger.jsonl
QString defaultLedgerPath();

// ISO-8601 Z stamp / parse helpers for the controller's ts handling.
QString nowIso8601();
qint64  parseIso8601Ms(const QString &ts);

}  // namespace ModelSwitchLedger
