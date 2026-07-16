#ifndef ANTS_MCPSPILL_H
#define ANTS_MCPSPILL_H

// ANTS-2094 — proactive server-side result offload (observation masking).
//
// When an offload-eligible MCP read verb returns a body over the byte
// threshold, the dispatcher spills the full body to a content-addressed
// cache file under ~/.cache/ants-terminal/mcp-spill/<sha256>.json and
// returns a small {head, handle, bytes, ...} envelope instead. The agent
// re-reads the full body (byte-paged) via the `read_spill` verb only when
// it needs more than the head. See docs/specs/ANTS-2094.md.
//
// Pure Qt6::Core (no widgets) so it links into ants_core_lib. The cache is
// global and content-addressed — never project-scoped — so it cannot
// "shadow" across a project relocation (docs/standards/mcp-caches.md).

#include <QJsonArray>
#include <QString>

class QJsonObject;

namespace mcp {

// Eviction caps (INV-7): whichever binds first. Oldest-mtime first, never
// dropping the handle written this call.
constexpr int     kSpillMaxFiles = 64;
constexpr qint64  kSpillMaxBytes = 64LL * 1024 * 1024;   // 64 MiB

// ANTS-3538 — structured preview for array-shaped bodies (§ 2.3.1 / INV-13).
// kHeadRowsMax caps head_rows cardinality independently of the byte budget;
// kStructuredParseMaxBytes skips the transient QJsonDocument::fromJson parse
// for bodies over the cap (RAM bound on the earlyoom host, § 4).
constexpr int     kHeadRowsMax           = 25;
constexpr qint64  kStructuredParseMaxBytes = 1LL * 1024 * 1024;  // 1 MiB

// ANTS-3545 — read_spill row-paging default page (rows) when row_count is
// omitted / 0. Row-count-bounded (not byte-bounded); still hard-capped by the
// kStructuredParseMaxBytes parse cap on the whole body. See docs/specs/ANTS-2094.md § 2.4.1.
constexpr int     kSpillRowsDefault      = 100;

// Config (set from the GUI thread on load / Settings Apply, read on every
// dispatch). setOffloadConfig clamps threshold to [4096, 1048576] and head
// to [256, 16384] (INV-12) before publishing.
void setOffloadConfig(bool enabled, int thresholdBytes, int headBytes);
bool offloadDefault();
int  offloadThresholdBytes();
int  offloadHeadBytes();

// Resolve whether offload is requested for this call: a per-call `offload`
// bool wins (true OR false), else the session default (mirrors ANTS-2085's
// `compact` resolution).
bool offloadRequested(const QJsonObject &args);

// ANTS-3552 — the dispatch-site offload size gate (INV-1), extracted so the
// >=threshold / >head boundary is behaviourally testable. offloadBody has no
// internal threshold guard (it always spills), so the dispatch must gate its
// call through this predicate. True iff bodyBytes >= offloadThresholdBytes()
// AND bodyBytes > offloadHeadBytes().
bool shouldOffload(qint64 bodyBytes);

// Spill `body` and return the head+pointer envelope JSON string. On ANY
// write failure (dir unwritable, disk full, commit() false) returns `body`
// unchanged — fail-open (INV-11). Caller must already have checked the
// threshold + head guard (INV-1) via shouldOffload().
QString offloadBody(const QString &toolName, const QString &body);

// read_spill byte-paging slice (INV-5/INV-6).
struct SpillSlice {
    bool    ok = false;
    QString code;          // bad_args / not_found when !ok
    QString content;       // UTF-8 char-boundary slice [offset, offset+n)
    qint64  offset = 0;
    qint64  bytes = 0;     // == content UTF-8 length
    qint64  totalBytes = 0;
    bool    truncated = false;
};
// handle must be a bare 64-hex sha256 (validated by the caller). maxBytes<=0
// → default 512 KiB; clamped to the 4 MiB ceiling.
SpillSlice readSpill(const QString &handle, qint64 offset, qint64 maxBytes);

// ANTS-3545 — read_spill structured row-paging (§ 2.4.1 / INV-14). Pages the
// spilled body's dominant array (the SAME array offloadBody previews as
// head_rows_key — shared detection) by ROW, parsed. `code` carries the refusal
// token when !ok; `cmdReadSpill` owns the human error/hint strings.
struct SpillRows {
    bool       ok = false;
    QString    code;          // bad_args / not_found / too_large / not_array
    QString    key;           // dominant array field name (== head_rows_key)
    QJsonArray rows;          // parsed [rowOffset, rowOffset+count) window
    qint64     rowOffset = 0;
    qint64     totalRows = 0; // dominant array's full length
    bool       truncated = false;
};
// handle must be a bare 64-hex sha256 (validated by the caller). Negative
// rowOffset/rowCount → bad_args; rowCount<=0 → kSpillRowsDefault. Stats the
// file and refuses too_large (> kStructuredParseMaxBytes) BEFORE reading it.
SpillRows readSpillRows(const QString &handle, qint64 rowOffset, qint64 rowCount);

// Session-start sweep: drop spill files with mtime older than 24 h (INV-7).
void spillSweep();

// Test seam (ANTS-2154): redirect the spill cache root so each test process
// owns a unique directory. The default path (GenericCacheLocation) has no
// per-process component, so under parallel ctest concurrent test binaries
// share one spill dir and the count/existence assertions race. Pass a unique
// per-test QTemporaryDir path here in SetUp; pass an empty string to restore
// the default. Production never calls this.
void setSpillDirOverride(const QString &dir);

}  // namespace mcp

#endif  // ANTS_MCPSPILL_H
