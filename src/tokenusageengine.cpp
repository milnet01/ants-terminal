// ANTS-1284 — implementation. See header and docs/specs/ANTS-1284.md.

#include "tokenusageengine.h"

#include <QDateTime>

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
    };
    return kBaselines;
}

constexpr qint64 kCharsPerToken = 4;  // ~Anthropic BPE order-of-magnitude

}  // namespace

Tracker::Tracker() {
    m_sinceUnixMs = QDateTime::currentMSecsSinceEpoch();
}

void Tracker::recordCall(const QString &toolName,
                         qint64         bytesIn,
                         qint64         bytesOut,
                         qint64         wrapBytes,
                         qint64         durationUs) {
    auto &c = m_counters[toolName];
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

        const qint64 baseline = baselineFor(r.tool);
        // Per-call baseline × n_calls is the modelled "would-have-spent"
        // cost; subtract observed wire bytes for the same period; floor
        // at 0 per INV-4 (negative would mean response exceeded model).
        const qint64 totalBaseline = baseline * r.nCalls;
        const qint64 totalActual   = r.bytesIn + r.bytesOut;
        const qint64 savedBytes    = std::max<qint64>(0, totalBaseline - totalActual);
        r.estTokensSaved = savedBytes / kCharsPerToken;

        snap.totalSaved     += r.estTokensSaved;
        snap.totalWrapBytes += r.wrapBytes;  // ANTS-1355 — across ALL tools
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
            if (r.estTokensSaved > 0) {
                snap.calls.append(std::move(r));
            }
        }
    }
    return snap;
}

}  // namespace TokenUsageEngine
