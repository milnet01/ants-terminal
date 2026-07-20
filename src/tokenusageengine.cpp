// ANTS-1284 — implementation. See header and docs/specs/ANTS-1284.md.

#include "tokenusageengine.h"

#include <QDateTime>
#include <QStringList>

#include <algorithm>

namespace TokenUsageEngine {

namespace {

// Per-tool baselines (bytes/call) — the estimated cost of doing the
// same work via Bash + Read instead of the MCP tool. See spec § 2.4
// for the anchors behind each number. Adding an entry here is a
// one-line change that flows directly through estTokensSaved math.
const QHash<QString, qint64> &baselineTable() {
    static const QHash<QString, qint64> kBaselines = {
        {QStringLiteral("roadmap_query"),  594000},   // ROADMAP.md size
        {QStringLiteral("verify_changes"),   8192},   // skill 4.1 KiB + ~4 KiB bash overhead
        {QStringLiteral("plan_template"),    8192},   // skill 6.0 KiB + ~2 KiB template echo
        // ANTS-3361 — the read/search verbs each REPLACE a full-file Read
        // or a grep, so they carry a real saving the meter previously
        // credited at ~0 (no baseline → estTokensSaved 0). Unlike
        // roadmap_query's exact file size, these are DELIBERATELY
        // CONSERVATIVE per-call estimates of the naive alternative's cost
        // (a modest source file / grep output) — a rough order-of-magnitude
        // model, matching the metric's existing precision. Under-shooting is
        // intentional: the estTokensSaved floor-at-0 (see buildReport)
        // means a call whose own output exceeds the baseline credits 0
        // rather than over-claiming, so we bias low and never inflate the
        // "tokens saved" headline. A precise per-call baseline (file size
        // threaded from each verb) is the larger cross-verb estimation task
        // tracked separately; these constants are the proportionate fix.
        {QStringLiteral("file_outline"),     8192},   // vs a full-file Read
        {QStringLiteral("read_region"),      8192},   // vs Read-ing the file to slice it
        {QStringLiteral("read_regions"),    12288},   // multiple slices / files
        {QStringLiteral("workspace_search"), 4096},   // vs grep -r output
        {QStringLiteral("codebase_index"),  12288},   // vs a project-wide grep/find
        {QStringLiteral("find_definition"),  4096},   // vs multi-grep for a def
        {QStringLiteral("find_sources"),     4096},   // vs multi-grep for callers
        {QStringLiteral("find_caller"),      4096},   // vs multi-grep for callers
        {QStringLiteral("build_status"),     3072},   // vs reading build-log tail
    };
    return kBaselines;
}

// ANTS-3579 — kCharsPerToken moved to the public header (tokenusageengine.h) so
// the per-project display path + tests reference it by symbol.

}  // namespace

Tracker::Tracker() {
    m_sinceUnixMs = QDateTime::currentMSecsSinceEpoch();
}

void Tracker::recordCall(const QString &toolName,
                         qint64         bytesIn,
                         qint64         bytesOut,
                         qint64         wrapBytes,
                         qint64         durationUs,
                         bool           success) {
    auto &c = m_counters[toolName];
    // ANTS-1432 — failed-call branch is mutually exclusive with the
    // success accumulator. We deliberately do NOT update nCalls /
    // durations / wrapBytes on failure: those are "what did this
    // tool cost when it worked" metrics; mixing failure bytes into
    // estTokensSaved arithmetic would muddy the saved figure.
    if (!success) {
        c.failedCalls    += 1;
        c.failedBytesIn  += bytesIn;
        c.failedBytesOut += bytesOut;
        return;
    }
    // ANTS-1355 INV-4: sentinel handling for min/max — overwrite
    // unconditionally on the first record, then min/max thereafter.
    if (c.nCalls == 0) {
        c.durationUsMin = durationUs;
        c.durationUsMax = durationUs;
    } else {
        if (durationUs < c.durationUsMin) c.durationUsMin = durationUs;
        if (durationUs > c.durationUsMax) c.durationUsMax = durationUs;
    }
    c.nCalls        += 1;
    c.bytesIn       += bytesIn;
    c.bytesOut      += bytesOut;
    c.wrapBytes     += wrapBytes;
    c.durationUsSum += durationUs;
}

void Tracker::reset() {
    m_counters.clear();
    m_sinceUnixMs = QDateTime::currentMSecsSinceEpoch();
}

qint64 Tracker::baselineFor(const QString &toolName) {
    const auto &t = baselineTable();
    auto it = t.find(toolName);
    return it == t.end() ? 0 : it.value();
}

Snapshot Tracker::buildReport(bool includeZero) const {
    Snapshot snap;
    snap.sinceUnixMs = m_sinceUnixMs;
    snap.toolsCalled = m_counters.size();

    QList<ToolReport> all;
    all.reserve(m_counters.size());
    for (auto it = m_counters.cbegin(); it != m_counters.cend(); ++it) {
        ToolReport r;
        r.tool          = it.key();
        r.nCalls        = it.value().nCalls;
        r.bytesIn       = it.value().bytesIn;
        r.bytesOut      = it.value().bytesOut;
        // ANTS-1355 v2 fields.
        r.wrapBytes     = it.value().wrapBytes;
        r.durationUsMin = it.value().durationUsMin;
        r.durationUsMax = it.value().durationUsMax;
        r.durationUsMean = (it.value().nCalls > 0)
            ? (it.value().durationUsSum / it.value().nCalls)
            : 0;
        // ANTS-1432 v3 fields.
        r.failedCalls    = it.value().failedCalls;
        r.failedBytesIn  = it.value().failedBytesIn;
        r.failedBytesOut = it.value().failedBytesOut;

        const qint64 baseline = baselineFor(r.tool);
        // Per-call baseline × n_calls is the modelled "would-have-spent"
        // cost; subtract observed wire bytes for the same period; floor
        // at 0 per INV-4 (negative would mean response exceeded model).
        const qint64 totalBaseline = baseline * r.nCalls;
        const qint64 totalActual   = r.bytesIn + r.bytesOut;
        const qint64 savedBytes    = std::max<qint64>(0, totalBaseline - totalActual);
        r.estTokensSaved = savedBytes / kCharsPerToken;

        snap.totalSaved        += r.estTokensSaved;
        snap.totalWrapBytes    += r.wrapBytes;     // ANTS-1355 — across ALL tools
        // ANTS-1432 — sum across ALL tools (even ones filtered out
        // of calls[] by include_zero) so the envelope summary stays
        // truthful regardless of the include_zero filter.
        snap.totalFailedBytes  += r.failedBytesIn + r.failedBytesOut;
        all.append(r);
    }

    // Sort by estTokensSaved desc, tiebreak by tool name asc — stable
    // ordering matters for the consumer (deterministic for tests, and
    // for diffing successive reports).
    std::sort(all.begin(), all.end(),
              [](const ToolReport &a, const ToolReport &b) {
        if (a.estTokensSaved != b.estTokensSaved) {
            return a.estTokensSaved > b.estTokensSaved;
        }
        return a.tool < b.tool;
    });

    if (includeZero) {
        snap.calls = std::move(all);
    } else {
        snap.calls.reserve(all.size());
        for (auto &r : all) {
            // ANTS-1432 — also retain tools with only failed calls
            // (estTokensSaved == 0 but failedCalls > 0). Surfacing
            // failure-only tools is the whole point of the metric.
            if (r.estTokensSaved > 0 || r.failedCalls > 0) {
                snap.calls.append(std::move(r));
            }
        }
    }
    return snap;
}

// ---- ANTS-3572 pure persistence helpers ---------------------------------

QJsonObject foldMonthlyBucket(QJsonObject monthly, const QString &monthKey,
                              qint64 add, int keepMonths) {
    // Values are stored as JSON numbers (double), exact-integer to 2^53 —
    // far beyond any real token total (INV-9).
    const qint64 prev = static_cast<qint64>(monthly.value(monthKey).toDouble(0));
    monthly[monthKey] = static_cast<double>(prev + add);

    // Retain only the keepMonths lexicographically-greatest keys. QJsonObject
    // keys() is ascending, so the oldest are at the front (INV-4).
    if (keepMonths > 0 && monthly.size() > keepMonths) {
        const QStringList keys = monthly.keys();  // ascending
        const int drop = monthly.size() - keepMonths;
        for (int i = 0; i < drop; ++i) monthly.remove(keys.at(i));
    }
    return monthly;
}

// ANTS-3579 — see header. Folds one root; does NOT evict (pruneProjectBuckets does).
QJsonObject foldProjectBucket(QJsonObject byProject, const QString &root,
                              qint64 addTokens, const QString &monthKey,
                              const QString &nowIso, int keepMonths) {
    QJsonObject bucket = byProject.value(root).toObject();
    const qint64 prevLife = static_cast<qint64>(
        bucket.value(QStringLiteral("lifetime")).toDouble(0));
    bucket[QStringLiteral("lifetime")] =
        static_cast<double>(prevLife + addTokens);
    bucket[QStringLiteral("monthly")] = foldMonthlyBucket(
        bucket.value(QStringLiteral("monthly")).toObject(), monthKey, addTokens,
        keepMonths);
    if (bucket.value(QStringLiteral("since")).toString().isEmpty())
        bucket[QStringLiteral("since")] = nowIso.left(10);  // date portion
    bucket[QStringLiteral("updated")] = nowIso;
    byProject[root] = bucket;
    return byProject;
}

// ANTS-3579 — evict to keepProjects roots, oldest `updated` first; ties broken by
// root string (larger evicted first) so the result is a pure function of the map.
QJsonObject pruneProjectBuckets(QJsonObject byProject, int keepProjects) {
    if (keepProjects <= 0 || byProject.size() <= keepProjects) return byProject;
    QStringList roots = byProject.keys();
    std::sort(roots.begin(), roots.end(),
              [&](const QString &a, const QString &b) {
                  const QString ua = byProject.value(a).toObject()
                                         .value(QStringLiteral("updated")).toString();
                  const QString ub = byProject.value(b).toObject()
                                         .value(QStringLiteral("updated")).toString();
                  if (ua != ub) return ua < ub;  // oldest updated evicted first
                  return a > b;                  // tie: larger root evicted first
              });
    const int drop = byProject.size() - keepProjects;
    for (int i = 0; i < drop; ++i) byProject.remove(roots.at(i));
    return byProject;
}

qint64 sumYear(const QJsonObject &monthly, const QString &yearPrefix) {
    qint64 sum = 0;
    for (auto it = monthly.begin(); it != monthly.end(); ++it) {
        if (it.key().startsWith(yearPrefix))
            sum += static_cast<qint64>(it.value().toDouble(0));
    }
    return sum;
}

QString humanizeCount(qint64 n) {
    if (n < 1000) return QString::number(n);
    double v;
    QLatin1Char suffix('K');
    if (n < 1000000) {
        v = n / 1000.0;
    } else if (n < 1000000000) {
        v = n / 1000000.0;
        suffix = QLatin1Char('M');
    } else {
        v = n / 1000000000.0;
        suffix = QLatin1Char('B');
    }
    QString s = QString::number(v, 'f', 1);
    if (s.endsWith(QLatin1String(".0"))) s.chop(2);  // "1.0K" → "1K"
    return s + suffix;
}

}  // namespace TokenUsageEngine
