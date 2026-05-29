// ANTS-1894 — see modelnearmissledger.h.
#include "modelnearmissledger.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include "modelswitchledger.h"   // parseIso8601Ms reuse (INV per § 2.3 helpers)
#include "secureio.h"            // setOwnerOnlyPerms

namespace ModelNearMissLedger {

namespace {

// Canonical blocker taxonomy in evaluation order (matches ModelAutoSwitch::
// decide() — INV-2 / INV-9). Used for tie-breaking in statsSlim. ANTS-1917
// appended `idle_end_of_session` as the 8th token (kept last so the v1 7-token
// ordering is never renumbered).
const QStringList &taxonomyOrder() {
    static const QStringList order = {
        QStringLiteral("auto_switch_disabled"),
        QStringLiteral("focused_state_not_idle"),
        QStringLiteral("composer_not_empty"),
        QStringLiteral("target_equals_current"),
        QStringLiteral("ticks_target_stable_insufficient"),
        QStringLiteral("dwell_time_insufficient"),
        QStringLiteral("override_cooldown_active"),
        QStringLiteral("idle_end_of_session"),
    };
    return order;
}

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
    setOwnerOnlyPerms(sf);
    return sf.commit();
}

}  // namespace

QString defaultLedgerPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/ants-terminal/model-switch-nearmiss.jsonl");
}

QJsonObject toJson(const Record &r) {
    QJsonObject o;
    o[QStringLiteral("ts")]                  = r.ts;
    o[QStringLiteral("session_id")]          = r.sessionId;
    o[QStringLiteral("project")]             = r.project;
    o[QStringLiteral("current_tier")]        = r.currentTier;
    o[QStringLiteral("recommended_tier")]    = r.recommendedTier;
    QJsonArray bb;
    for (const QString &t : r.blockedBy) bb.append(t);
    o[QStringLiteral("blocked_by")]          = bb;
    o[QStringLiteral("composer_empty")]      = r.composerEmpty;
    o[QStringLiteral("focused_state")]       = r.focusedState;
    o[QStringLiteral("ticks_target_stable")] = r.ticksTargetStable;
    o[QStringLiteral("dwell_ms")]            = static_cast<double>(r.dwellMs);
    o[QStringLiteral("ms_since_last_override")] =
        static_cast<double>(r.msSinceLastOverride);
    return o;
}

Record fromJson(const QJsonObject &o) {
    Record r;
    r.ts                  = o.value(QStringLiteral("ts")).toString();
    r.sessionId           = o.value(QStringLiteral("session_id")).toString();
    r.project             = o.value(QStringLiteral("project")).toString();
    r.currentTier         = o.value(QStringLiteral("current_tier")).toString();
    r.recommendedTier     = o.value(QStringLiteral("recommended_tier")).toString();
    const QJsonArray bb   = o.value(QStringLiteral("blocked_by")).toArray();
    for (const auto &v : bb) r.blockedBy.append(v.toString());
    // INV-13 — missing keys default to the struct defaults.
    r.composerEmpty       = o.value(QStringLiteral("composer_empty")).toBool(true);
    r.focusedState        = o.value(QStringLiteral("focused_state")).toString();
    r.ticksTargetStable   = o.value(QStringLiteral("ticks_target_stable")).toInt(0);
    r.dwellMs             = static_cast<qint64>(
        o.value(QStringLiteral("dwell_ms")).toDouble(0.0));
    r.msSinceLastOverride = o.contains(QStringLiteral("ms_since_last_override"))
        ? static_cast<qint64>(
              o.value(QStringLiteral("ms_since_last_override")).toDouble(-1.0))
        : -1;
    return r;
}

QList<QByteArray> evictToCap(QList<QByteArray> lines, qint64 capBytes) {
    auto total = [&lines]() -> qint64 {
        qint64 t = 0;
        for (const QByteArray &l : lines) t += l.size() + 1;
        return t;
    };
    // INV-8 — drop oldest non-newest lines until ≤ capBytes; never drop the
    // newest line; lines removed whole (no mid-line truncation). No
    // pending-pinning (near-miss has no outcome backfill).
    while (total() > capBytes && lines.size() > 1) {
        lines.removeFirst();
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

namespace {

// Filter records by (24h window relative to nowMs) AND (project match when
// projectScope is non-empty). Empty projectScope → no project filter (global).
QList<Record> filterWindow(const QList<Record> &recs,
                           const QString       &projectScope,
                           qint64               nowMs) {
    QList<Record> out;
    for (const Record &r : recs) {
        if (!projectScope.isEmpty() && r.project != projectScope) continue;
        if (nowMs > 0) {
            const qint64 tsMs = ModelSwitchLedger::parseIso8601Ms(r.ts);
            if (tsMs <= 0) continue;                      // malformed ts → skip
            if (nowMs - tsMs > kStatsWindowMs) continue;  // outside window
        }
        out.append(r);
    }
    return out;
}

QList<Record> filterProject(const QList<Record> &recs,
                            const QString       &projectScope) {
    if (projectScope.isEmpty()) return recs;
    QList<Record> out;
    for (const Record &r : recs)
        if (r.project == projectScope) out.append(r);
    return out;
}

// Per-token occurrence count (each record contributes one count per distinct
// token in its blocked_by array).
QHash<QString, int> tallyBlockedBy(const QList<Record> &recs) {
    QHash<QString, int> out;
    for (const Record &r : recs) {
        QStringList seen;
        for (const QString &t : r.blockedBy) {
            if (seen.contains(t)) continue;   // de-dup within a single record
            seen.append(t);
            out[t] = out.value(t, 0) + 1;
        }
    }
    return out;
}

QString dominantBlocker(const QHash<QString, int> &tally) {
    if (tally.isEmpty()) return QString();
    int bestCount = -1;
    QString best;
    int bestOrder = -1;
    const QStringList &order = taxonomyOrder();
    for (auto it = tally.constBegin(); it != tally.constEnd(); ++it) {
        const int taxIdx = order.indexOf(it.key());
        if (it.value() > bestCount
            || (it.value() == bestCount
                && taxIdx >= 0
                && (bestOrder < 0 || taxIdx < bestOrder))) {
            bestCount = it.value();
            best      = it.key();
            bestOrder = taxIdx;
        }
    }
    return best;
}

QJsonObject tallyToJson(const QHash<QString, int> &tally) {
    QJsonObject out;
    for (auto it = tally.constBegin(); it != tally.constEnd(); ++it)
        out[it.key()] = it.value();
    return out;
}

// Count distinct post-sort blocked_by arrays.
int distinctSignatures(const QList<Record> &recs) {
    QStringList sigs;
    for (const Record &r : recs) {
        QStringList sig = r.blockedBy;
        std::sort(sig.begin(), sig.end());
        const QString joined = sig.join(QLatin1Char('|'));
        if (!sigs.contains(joined)) sigs.append(joined);
    }
    return sigs.size();
}

}  // namespace

StatsSlim statsSlim(const QList<Record> &recs,
                    const QString       &projectScope,
                    qint64               nowMs) {
    const QList<Record> windowed = filterWindow(recs, projectScope, nowMs);
    StatsSlim out;
    out.total24h        = windowed.size();
    out.dominantBlocker = dominantBlocker(tallyBlockedBy(windowed));
    return out;
}

QJsonObject statsFull(const QList<Record> &recs,
                      const QString       &projectScope,
                      qint64               nowMs) {
    const QList<Record> all      = filterProject(recs, projectScope);
    const QList<Record> windowed = filterWindow(recs, projectScope, nowMs);

    const QHash<QString, int> tallyWindow = tallyBlockedBy(windowed);
    const QHash<QString, int> tallyAll    = tallyBlockedBy(all);
    const QString domWindow               = dominantBlocker(tallyWindow);

    QJsonObject window24h;
    window24h[QStringLiteral("total")]                = windowed.size();
    window24h[QStringLiteral("by_blocked_by")]        = tallyToJson(tallyWindow);
    window24h[QStringLiteral("dominant_blocker")]     = domWindow;
    window24h[QStringLiteral("distinct_signatures")]  = distinctSignatures(windowed);

    QJsonObject allTime;
    allTime[QStringLiteral("total")]                  = all.size();
    allTime[QStringLiteral("by_blocked_by")]          = tallyToJson(tallyAll);
    allTime[QStringLiteral("distinct_signatures")]    = distinctSignatures(all);

    QJsonObject env;
    env[QStringLiteral("ok")]          = true;
    env[QStringLiteral("mode")]        = QStringLiteral("near_misses");
    env[QStringLiteral("scope")]       = projectScope.isEmpty()
                                            ? QStringLiteral("global")
                                            : QStringLiteral("project");
    env[QStringLiteral("window_24h")]  = window24h;
    env[QStringLiteral("all_time")]    = allTime;

    // Headline — human-readable summary of the 24 h window.
    QString headline;
    if (windowed.isEmpty()) {
        headline = QStringLiteral("0 near-misses in last 24 h");
    } else {
        const int n   = windowed.size();
        const int dom = tallyWindow.value(domWindow, 0);
        const int pct = (n > 0) ? (100 * dom + n / 2) / n : 0;
        headline = QStringLiteral("%1 near-miss%2 in last 24 h, dominated by %3 (%4%)")
                       .arg(n)
                       .arg(n == 1 ? QString() : QStringLiteral("es"))
                       .arg(domWindow)
                       .arg(pct);
    }
    env[QStringLiteral("headline")] = headline;
    return env;
}

}  // namespace ModelNearMissLedger
