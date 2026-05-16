// ANTS-1436 — implementation. See header + docs/specs/ANTS-1436.md.

#include "paginationengine.h"

#include <QJsonDocument>

namespace PaginationEngine {

namespace {

// Measure-then-cut binary search: find the largest n such that
// serializing filtered.mid(0, n) fits under `budget` bytes. Returns
// 0 if even one bullet exceeds the budget (caller emits an empty
// slice with next_offset:0).
int measureCutPoint(const QJsonArray &filtered, int budget) {
    if (filtered.isEmpty()) return 0;
    // Fast path: full array fits.
    {
        QJsonArray a;
        for (const auto &v : filtered) a.append(v);
        const auto bytes = QJsonDocument(a).toJson(QJsonDocument::Compact).size();
        if (bytes <= budget) return filtered.size();
    }
    // Binary search the cut.
    int lo = 0;
    int hi = filtered.size();
    while (lo < hi) {
        const int mid = lo + (hi - lo + 1) / 2;  // ceiling midpoint
        QJsonArray probe;
        for (int i = 0; i < mid; ++i) probe.append(filtered.at(i));
        const auto bytes = QJsonDocument(probe).toJson(QJsonDocument::Compact).size();
        if (bytes <= budget) lo = mid;       // mid fits, try larger
        else                 hi = mid - 1;   // mid too big, shrink
    }
    return lo;
}

}  // namespace

PageResult pageBullets(const QJsonArray &filtered,
                       int offset,
                       int limit) {
    PageResult r;
    r.total = filtered.size();

    // Offset clamping. Past-end returns empty slice with offset
    // equal to total (caller treats it as "no more pages").
    if (offset < 0) offset = 0;
    if (offset > r.total) offset = r.total;
    r.offset = offset;

    const QJsonArray tail = (offset == 0)
        ? filtered
        : [&]() {
              QJsonArray t;
              for (int i = offset; i < r.total; ++i) t.append(filtered.at(i));
              return t;
          }();

    // Limit handling. -1 = auto-pick (caller didn't pass). Explicit
    // value clamps to [kMinLimit, kMaxLimit].
    if (limit > 0) {
        if (limit < kMinLimit) limit = kMinLimit;
        if (limit > kMaxLimit) limit = kMaxLimit;
        // Take the first `limit` of tail.
        QJsonArray slice;
        for (int i = 0; i < tail.size() && i < limit; ++i) {
            slice.append(tail.at(i));
        }
        r.slice = slice;
        r.limit = limit;
    } else {
        // Auto-pick: measure-then-cut against the soft cap. Reserve
        // envelope overhead so the wire response fits comfortably.
        const int budget = kSoftCapBytes - kEnvelopeOverheadBytes;
        const int n = measureCutPoint(tail, budget);
        QJsonArray slice;
        for (int i = 0; i < n; ++i) slice.append(tail.at(i));
        r.slice = slice;
        r.limit = n;  // echo the effective auto-picked limit
    }

    r.truncated = (r.slice.size() < tail.size());
    if (r.truncated) {
        r.nextOffset = offset + r.slice.size();
    }
    return r;
}

bool shouldEmitPaginationFields(bool callerPassedOffset,
                                bool callerPassedLimit,
                                bool serverAutoTruncated) {
    return callerPassedOffset
        || callerPassedLimit
        || serverAutoTruncated;
}

}  // namespace PaginationEngine
