// ANTS-1436 — pagination + auto-truncate helper for cmdRoadmapQuery.
// Pure (Qt6::Core-only); lives in ants_core_lib. Stateless — args
// in, slice + envelope-augment hints out. See docs/specs/ANTS-1436.md.

#pragma once

#include <QJsonArray>
#include <QString>

#include <functional>

namespace PaginationEngine {

// ANTS-3543 — optional row projector. Mutates a row array in place to its
// lean shape (same contract as rcProjectHeadlineOnly). Qt6::Core-only.
using RowProjector = std::function<void(QJsonArray &)>;

// Soft cap for the bullets[] subset emitted in one response. Set
// below the MCP client's 25k-token spill threshold so the wire
// envelope (bullets + wrapper fields + ANTS-1294 framing) fits
// comfortably under cap. Cold-eyes review 2026-05-16 H3 — measured
// against this repo's ROADMAP.md.
constexpr int kSoftCapBytes = 20480;

// Envelope wrapper overhead — ok/path/count/filter/section/format
// fields + the ANTS-1294 <ants_mcp_data> framing. Conservative
// enough that bullets[] always has 20 KB of its own budget.
constexpr int kEnvelopeOverheadBytes = 512;

// Hard clamps on caller-supplied args, per spec INV-8/INV-9.
constexpr int kMaxLimit = 500;
constexpr int kMinLimit = 1;

// Result of a pageBullets call.
struct PageResult {
    QJsonArray slice;        // bullets to emit in this response
    int        offset = 0;   // echoed-back offset (post-clamp)
    int        limit = 0;    // effective limit applied (post-clamp + auto-pick)
    int        total = 0;    // post-filter total before pagination
    bool       truncated = false;  // slice.size() < (total - offset)
    int        nextOffset = -1;    // valid iff truncated; else -1
    bool       downshifted = false;  // ANTS-3543 — rows projected to their
                                     // lean shape to fit more items in budget.
};

// Slice `filtered` per `offset` + `limit`, applying auto-truncate
// (measure-then-cut binary search) when `limit < 0` (sentinel meaning
// "caller omitted limit; auto-pick"). Returns a PageResult ready to
// fold into the cmdRoadmapQuery envelope.
//
// Argument semantics:
// - `filtered`: post-status, post-rollup bullet array (caller has
//   already applied those passes).
// - `offset`: 0-based start index. Clamped to [0, filtered.size()].
//   Past-end is allowed; returns empty slice with offset == total.
// - `limit`: caller's explicit limit (clamped to [kMinLimit,
//   kMaxLimit]) when >= 1; pass -1 to request auto-pick (caller
//   omitted the arg). 0 is invalid and the caller must reject
//   upstream with bad_args before calling this helper.
// - `downshift` (ANTS-3543): optional lean projector. When set AND the
//   auto path (limit <= 0) truncated the fat set, re-pages a lean copy of
//   the FULL `filtered` so a scanning caller keeps every row's identity
//   instead of losing the tail (PageResult.downshifted = true). Default {}
//   → byte-identical to pre-3543 behaviour. Never mutates `filtered`.
PageResult pageBullets(const QJsonArray &filtered,
                       int offset,
                       int limit,
                       const RowProjector &downshift = {});

// True when the caller-supplied (offset, limit) pair should produce
// pagination fields on the envelope. Reasoning: pre-1436 callers
// shouldn't see the new fields, so emission is opt-in. Pagination
// fields are emitted when:
//   - caller passed offset OR limit explicitly, OR
//   - server auto-truncated (filtered exceeded the soft cap).
bool shouldEmitPaginationFields(bool callerPassedOffset,
                                bool callerPassedLimit,
                                bool serverAutoTruncated);

}  // namespace PaginationEngine
