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
    // ANTS-2200 — serialize each element exactly once, then accumulate. The old
    // binary search re-serialized a growing `mid`-element prefix on every probe
    // (O(n log n) bytes serialized just to size one page); this is O(n).
    //
    // Compact-array bytes for the first n elements (n>=1) are:
    //   2 (the `[` `]` brackets) + (n-1) commas + sum(elemLen[0..n-1])
    // where elemLen[i] is the compact serialization length of element i — got by
    // serializing the singleton array `[elem]` (= "[" elem "]") and subtracting
    // the 2 bracket bytes. The running total is monotonic in n, so the first
    // element that overflows `budget` ends the prefix that fits.
    qint64 running = 0;   // compact bytes of filtered.mid(0, cut)
    int cut = 0;
    for (const auto &v : filtered) {
        QJsonArray one;
        one.append(v);
        const qint64 elemLen =
            QJsonDocument(one).toJson(QJsonDocument::Compact).size() - 2;
        // n==1: 2 brackets + elemLen; n>=2: prior total + 1 comma + elemLen.
        running = (cut == 0) ? (2 + elemLen) : (running + 1 + elemLen);
        if (running > budget) break;
        ++cut;
    }
    return cut;
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
