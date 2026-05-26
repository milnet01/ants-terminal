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

QJsonObject statsEnvelope(const QList<Record> &recs) {
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

    // Reported as a ratio, never the flattering numerator alone (MEDIUM-1).
    const QString headline = QStringLiteral(
        "avoided %1 Opus turns, %2 regretted (regret %3%)")
        .arg(opusAvoided).arg(regretCount)
        .arg(QString::number(regretRate, 'f', 1));

    QJsonObject env;
    env[QStringLiteral("ok")]                   = true;
    env[QStringLiteral("switches")]             = recs.size();
    env[QStringLiteral("downgrades")]           = downgrades;
    env[QStringLiteral("upgrades")]             = upgrades;
    env[QStringLiteral("opus_turns_avoided")]   = opusAvoided;
    env[QStringLiteral("opus_turns_routed_in")] = opusRoutedIn;
    env[QStringLiteral("regret_count")]         = regretCount;
    env[QStringLiteral("regret_rate")]          = regretRate;
    env[QStringLiteral("under_route_count")]    = underRouteCount;
    env[QStringLiteral("pending_count")]        = pendingCount;
    env[QStringLiteral("by_tier")]              = byTier;
    env[QStringLiteral("headline")]             = headline;
    return env;
}

QJsonObject statsForProject(const QString &ledgerPath, const QString &projectRoot) {
    QList<Record> scoped;
    for (const Record &r : readRecords(ledgerPath))   // absent ledger → empty
        if (r.project == projectRoot) scoped.append(r);
    return statsEnvelope(scoped);
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
