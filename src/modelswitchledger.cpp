// ANTS-1735 — see modelswitchledger.h.
#include "modelswitchledger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include "secureio.h"   // setOwnerOnlyPerms

namespace ModelSwitchLedger {

namespace {

QByteArray serialize(const Record &rec) {
    return QJsonDocument(toJson(rec)).toJson(QJsonDocument::Compact);
}

QList<QByteArray> readRawLines(const QString &path) {
    QList<QByteArray> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray all = f.readAll();
    for (const QByteArray &ln : all.split('\n'))
        if (!ln.trimmed().isEmpty()) out.append(ln);
    return out;
}

bool isPendingLine(const QByteArray &ln) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(ln, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    return doc.object().value(QStringLiteral("outcome")).toObject()
        .value(QStringLiteral("pending")).toBool(false);
}

bool writeLinesAtomic(const QString &path, const QList<QByteArray> &lines) {
    const QFileInfo fi(path);
    if (!QDir().mkpath(fi.absolutePath())) return false;
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) return false;
    for (const QByteArray &ln : lines) {
        if (sf.write(ln) != ln.size() || sf.write("\n", 1) != 1) {
            sf.cancelWriting();
            return false;
        }
    }
    // Tighten perms on the temp file before the atomic rename so the final
    // ledger is never briefly group/world-readable (no create-then-chmod window).
    setOwnerOnlyPerms(sf);
    return sf.commit();
}

}  // namespace

QJsonObject toJson(const Record &r) {
    QJsonObject oc;
    oc[QStringLiteral("turns_on_to_tier")]              = r.outcome.turnsOnToTier;
    oc[QStringLiteral("user_override_within_5_turns")]  = r.outcome.userOverrideWithin5;
    oc[QStringLiteral("correction_signal_within_5_turns")] = r.outcome.correctionSignalWithin5;
    oc[QStringLiteral("under_route_signal_within_5_turns")] = r.outcome.underRouteSignalWithin5;
    oc[QStringLiteral("pending")]                       = r.outcome.pending;

    QJsonObject o;
    o[QStringLiteral("ts")]           = r.ts;
    o[QStringLiteral("session_id")]   = r.sessionId;
    o[QStringLiteral("project")]      = r.project;
    o[QStringLiteral("from_tier")]    = r.fromTier;
    o[QStringLiteral("to_tier")]      = r.toTier;
    o[QStringLiteral("score_reason")] = r.scoreReason;
    o[QStringLiteral("trigger")]      = r.trigger;
    o[QStringLiteral("outcome")]      = oc;
    return o;
}

Record fromJson(const QJsonObject &o) {
    Record r;
    r.ts          = o.value(QStringLiteral("ts")).toString();
    r.sessionId   = o.value(QStringLiteral("session_id")).toString();
    r.project     = o.value(QStringLiteral("project")).toString();
    r.fromTier    = o.value(QStringLiteral("from_tier")).toString();
    r.toTier      = o.value(QStringLiteral("to_tier")).toString();
    r.scoreReason = o.value(QStringLiteral("score_reason")).toString();
    r.trigger     = o.value(QStringLiteral("trigger")).toString();
    const QJsonObject oc = o.value(QStringLiteral("outcome")).toObject();
    r.outcome.turnsOnToTier           = oc.value(QStringLiteral("turns_on_to_tier")).toInt();
    r.outcome.userOverrideWithin5     = oc.value(QStringLiteral("user_override_within_5_turns")).toBool();
    r.outcome.correctionSignalWithin5 = oc.value(QStringLiteral("correction_signal_within_5_turns")).toBool();
    r.outcome.underRouteSignalWithin5 = oc.value(QStringLiteral("under_route_signal_within_5_turns")).toBool();
    // Absent outcome → pending (a record with no measured outcome is in-flight,
    // hence pinned from eviction).
    r.outcome.pending = oc.value(QStringLiteral("pending")).toBool(true);
    return r;
}

QList<QByteArray> evictToCap(QList<QByteArray> lines, qint64 capBytes) {
    auto total = [&lines]() -> qint64 {
        qint64 t = 0;
        for (const QByteArray &l : lines) t += l.size() + 1;  // + '\n'
        return t;
    };
    while (total() > capBytes && lines.size() > 1) {
        int victim = -1;
        // Oldest non-pending line, never the newest (last) — pending records are
        // pinned, lines removed whole (no mid-line truncation). INV-10.
        for (int i = 0; i < lines.size() - 1; ++i) {
            if (!isPendingLine(lines.at(i))) { victim = i; break; }
        }
        if (victim < 0) break;   // only pinned records + newest remain
        lines.removeAt(victim);
    }
    return lines;
}

bool appendRecord(const QString &path, const Record &rec, qint64 capBytes) {
    QList<QByteArray> lines = readRawLines(path);
    lines.append(serialize(rec));
    lines = evictToCap(lines, capBytes);
    return writeLinesAtomic(path, lines);
}

QList<Record> readRecords(const QString &path) {
    QList<Record> out;
    for (const QByteArray &ln : readRawLines(path)) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(ln, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            out.append(fromJson(doc.object()));
    }
    return out;
}

bool writeRecords(const QString &path, const QList<Record> &recs, qint64 capBytes) {
    QList<QByteArray> lines;
    lines.reserve(recs.size());
    for (const Record &r : recs) lines.append(serialize(r));
    lines = evictToCap(lines, capBytes);
    return writeLinesAtomic(path, lines);
}

bool detectUserOverride(const QList<ModelEvent> &windowModelEvents,
                        const QList<AutoSwitch> &autoRecords,
                        qint64 authorWindowMs) {
    for (const ModelEvent &e : windowModelEvents) {
        bool autoAuthored = false;
        for (const AutoSwitch &a : autoRecords) {
            if (a.toTier == e.tier && qAbs(e.tsMs - a.tsMs) <= authorWindowMs) {
                autoAuthored = true;
                break;
            }
        }
        if (!autoAuthored) return true;   // a /model the controller didn't inject
    }
    return false;
}

UnderRoute detectUnderRoute(const QString &toTier,
                            const QStringList &subsequentRecommendedTiers) {
    if (subsequentRecommendedTiers.isEmpty()) return UnderRoute::Pending;
    const int base = tierRank(toTier);
    const int n = qMin(subsequentRecommendedTiers.size(), kOutcomeWindowTurns);
    for (int i = 0; i < n; ++i) {
        if (tierRank(subsequentRecommendedTiers.at(i)) > base) return UnderRoute::Yes;
    }
    return UnderRoute::No;
}

bool detectCorrection(const QString &firstUserTurnText) {
    // Linear alternation, no nested quantifiers → no catastrophic backtracking.
    static const QRegularExpression re(
        QStringLiteral("\\b(no|wrong|that's not|undo|revert|try again)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(firstUserTurnText).hasMatch();
}

QString aliasFromModelId(const QString &modelId) {
    if (modelId.isEmpty()) return {};
    if (modelId.contains(QStringLiteral("haiku"), Qt::CaseInsensitive))
        return QStringLiteral("haiku");
    if (modelId.contains(QStringLiteral("opus"),  Qt::CaseInsensitive))
        return QStringLiteral("opus");
    return QStringLiteral("sonnet");
}

namespace {

// Detect "/model X" prefix in a user turn's joined text content. Returns the
// alias ("haiku"/"sonnet"/"opus") or empty if not a model command.
QString parseUserModelCommand(const QString &userText) {
    static const QRegularExpression re(
        QStringLiteral("^\\s*/model\\s+(haiku|sonnet|opus)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(userText);
    return m.hasMatch() ? m.captured(1).toLower() : QString();
}

}  // namespace

OutcomeFillResult computeOutcome(
    const QString &fromTier, const QString &toTier,
    qint64 switchTsMs,
    const QList<TranscriptTurn> &postSwitchTurns,
    const QList<AutoSwitch> &subsequentAutoSwitches,
    const QStringList &subsequentRecommendedTiers,
    const Outcome &inputBefore)
{
    OutcomeFillResult res;
    res.outcome = inputBefore;
    res.outcome.pending = true;   // start pending; clear once settled

    const bool isDowngrade = (tierRank(fromTier) > tierRank(toTier))
                          && tierRank(fromTier) >= 0
                          && tierRank(toTier)   >= 0;

    // Slice turns to strictly post-switch (tsMs > switchTsMs). Pre-switch
    // turns may slip in when the caller passes a generous window — guard here.
    QList<TranscriptTurn> turns;
    turns.reserve(postSwitchTurns.size());
    for (const TranscriptTurn &t : postSwitchTurns)
        if (t.tsMs > switchTsMs) turns.append(t);

    // turns_on_to_tier: count contiguous assistant turns whose modelId aliases
    // to `toTier`. Stop at the first divergent assistant turn.
    int onTier = 0;
    int firstDivergentIdx = -1;
    for (int i = 0; i < turns.size(); ++i) {
        if (!turns[i].isAssistant) continue;
        if (aliasFromModelId(turns[i].modelId) == toTier) {
            ++onTier;
        } else {
            firstDivergentIdx = i;
            break;
        }
    }
    res.outcome.turnsOnToTier = onTier;

    // user_override scan — first kOutcomeWindowTurns user turns post-switch
    // whose text starts with "/model X". The INV-11 correlation removes any
    // /model X that an auto record authored within kAuthorWindowMs.
    QList<ModelEvent> userModelEvents;
    {
        int userTurnsSeen = 0;
        for (const TranscriptTurn &t : turns) {
            if (!t.isUser) continue;
            if (userTurnsSeen >= kOutcomeWindowTurns) break;
            ++userTurnsSeen;
            const QString cmd = parseUserModelCommand(t.userText);
            if (!cmd.isEmpty()) {
                ModelEvent ev;
                ev.tier = cmd;
                ev.tsMs = t.tsMs;
                userModelEvents.append(ev);
            }
        }
    }
    res.outcome.userOverrideWithin5 =
        detectUserOverride(userModelEvents, subsequentAutoSwitches);

    // correction signal — first post-switch user turn ONLY, downgrades ONLY.
    if (isDowngrade) {
        for (const TranscriptTurn &t : turns) {
            if (t.isUser) {
                res.outcome.correctionSignalWithin5 =
                    detectCorrection(t.userText);
                break;
            }
        }
    }

    // under_route signal — downgrades only.
    if (isDowngrade) {
        const UnderRoute ur = detectUnderRoute(toTier, subsequentRecommendedTiers);
        res.outcome.underRouteSignalWithin5 = (ur == UnderRoute::Yes);
    }

    // Settlement: dwell ended (subsequent auto-switch exists) OR ≥ kOutcomeWindowTurns
    // post-switch assistant turns observed OR turns_on_to_tier already capped by
    // a divergent assistant turn (a user-driven /model ended the dwell).
    const bool dwellEndedByNextAuto  = !subsequentAutoSwitches.isEmpty();
    const bool fullWindowObserved    = (onTier >= kOutcomeWindowTurns);
    const bool dwellEndedByDivergence = (firstDivergentIdx >= 0);
    if (dwellEndedByNextAuto || fullWindowObserved || dwellEndedByDivergence)
        res.outcome.pending = false;

    // changed flag — caller flushes only when something differs from input.
    res.changed = (res.outcome.pending != inputBefore.pending)
               || (res.outcome.turnsOnToTier != inputBefore.turnsOnToTier)
               || (res.outcome.userOverrideWithin5 != inputBefore.userOverrideWithin5)
               || (res.outcome.correctionSignalWithin5 != inputBefore.correctionSignalWithin5)
               || (res.outcome.underRouteSignalWithin5 != inputBefore.underRouteSignalWithin5);
    return res;
}

QJsonObject statsEnvelope(const QList<Record> &recs, const StatsConfig &cfg) {
    int downgrades = 0, upgrades = 0;
    int opusAvoided = 0, opusRoutedIn = 0;
    int regretCount = 0, underRouteCount = 0, pendingCount = 0;
    int measuredDowngrades = 0;   // non-pending downgrades — regret denominator
    int toHaiku = 0, toSonnet = 0, toOpus = 0;

    const QString haiku = QStringLiteral("haiku");
    const QString opus  = QStringLiteral("opus");

    for (const Record &r : recs) {
        const int rf = tierRank(r.fromTier);
        const int rt = tierRank(r.toTier);

        if (r.toTier == haiku)                       ++toHaiku;
        else if (r.toTier == QStringLiteral("sonnet")) ++toSonnet;
        else if (r.toTier == opus)                   ++toOpus;

        if (r.outcome.pending) ++pendingCount;

        const bool isDowngrade = (rf >= 0 && rt >= 0 && rt < rf);
        const bool isUpgrade   = (rf >= 0 && rt >= 0 && rt > rf);
        if (isDowngrade) ++downgrades;
        if (isUpgrade)   ++upgrades;

        // Turns on the cheaper tier after leaving Opus are Opus turns avoided;
        // turns on Opus after upgrading to it are Opus turns routed in.
        if (r.fromTier == opus && r.toTier != opus)
            opusAvoided += r.outcome.turnsOnToTier;
        if (r.toTier == opus && r.fromTier != opus)
            opusRoutedIn += r.outcome.turnsOnToTier;

        // Outcome signals count only on a *measured* (non-pending) downgrade —
        // pending records are reported separately, never as success or harm.
        if (isDowngrade && !r.outcome.pending) {
            ++measuredDowngrades;
            if (r.outcome.userOverrideWithin5 || r.outcome.correctionSignalWithin5)
                ++regretCount;
            if (r.outcome.underRouteSignalWithin5)
                ++underRouteCount;
        }
    }

    const double regretRate = measuredDowngrades > 0
        ? (100.0 * regretCount / measuredDowngrades) : 0.0;

    QJsonObject byTier;
    byTier[QStringLiteral("haiku")]  = toHaiku;
    byTier[QStringLiteral("sonnet")] = toSonnet;
    byTier[QStringLiteral("opus")]   = toOpus;

    // ANTS-1889 — headline branches on enable state + scope so the caller
    // can tell "feature OFF" from "feature ON, no candidates yet" from
    // "feature ON, measured outcomes". Reported as a ratio, never the
    // flattering numerator alone (MEDIUM-1).
    QString headline;
    if (!cfg.switchEnabled) {
        headline = QStringLiteral("auto-switch OFF");
    } else if (recs.isEmpty()) {
        headline = (cfg.scope == QStringLiteral("global"))
            ? QStringLiteral("auto-switch ON globally (no switches yet)")
            : QStringLiteral("auto-switch ON in this project (no switches yet)");
    } else {
        headline = QStringLiteral(
            "avoided %1 Opus turns, %2 regretted (regret %3%)")
            .arg(opusAvoided).arg(regretCount)
            .arg(QString::number(regretRate, 'f', 1));
    }

    QJsonObject env;
    env[QStringLiteral("ok")]                       = true;
    env[QStringLiteral("switches")]                 = recs.size();
    env[QStringLiteral("downgrades")]               = downgrades;
    env[QStringLiteral("upgrades")]                 = upgrades;
    env[QStringLiteral("opus_turns_avoided")]       = opusAvoided;
    env[QStringLiteral("opus_turns_routed_in")]     = opusRoutedIn;
    env[QStringLiteral("regret_count")]             = regretCount;
    env[QStringLiteral("regret_rate")]              = regretRate;
    env[QStringLiteral("under_route_count")]        = underRouteCount;
    env[QStringLiteral("pending_count")]            = pendingCount;
    env[QStringLiteral("by_tier")]                  = byTier;
    // ANTS-1889 — live switcher config triple + scope echo.
    env[QStringLiteral("auto_model_switch_enabled")] = cfg.switchEnabled;
    env[QStringLiteral("floor_tier")]                = cfg.floorTier;
    env[QStringLiteral("min_dwell_sec")]             = cfg.minDwellSec;
    env[QStringLiteral("scope")]                     = cfg.scope;
    env[QStringLiteral("headline")]                  = headline;
    return env;
}

QJsonObject statsForScope(const QString &ledgerPath,
                          const QString &projectRoot,
                          const StatsConfig &cfg) {
    const bool global = (cfg.scope == QStringLiteral("global"));
    QList<Record> scoped;
    for (const Record &r : readRecords(ledgerPath))   // absent ledger → empty
        if (global || r.project == projectRoot) scoped.append(r);
    return statsEnvelope(scoped, cfg);
}

// Back-compat thin wrapper — no config surfaced, project scope.
QJsonObject statsForProject(const QString &ledgerPath, const QString &projectRoot) {
    StatsConfig cfg;   // defaults: disabled, project scope
    return statsForScope(ledgerPath, projectRoot, cfg);
}

int tierRank(const QString &alias) {
    if (alias == QStringLiteral("haiku"))  return 0;
    if (alias == QStringLiteral("sonnet")) return 1;
    if (alias == QStringLiteral("opus"))   return 2;
    return -1;
}

QString defaultLedgerPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/ants-terminal/model-switch-ledger.jsonl");
}

QString nowIso8601() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

qint64 parseIso8601Ms(const QString &ts) {
    const QDateTime dt = QDateTime::fromString(ts, Qt::ISODate);
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

}  // namespace ModelSwitchLedger
