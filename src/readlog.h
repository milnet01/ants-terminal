// ANTS-1855 — read_log MCP tool: pure filtering helper. Qt6::Core-only,
// lives in ants_core_lib so it is unit-testable without RemoteControl /
// MainWindow (mirrors fileoutline.h / paginationengine.h). The thin
// cmdReadLog wrapper (remotecontrol.cpp) handles path resolution +
// PathValidation + the caller_cwd contract, then calls ReadLog::filter
// on the resolved path. See docs/specs/ANTS-1855.md.

#pragma once

#include <QJsonObject>
#include <QString>

namespace ReadLog {

// Soft caps shared with the read-tool family (see RemoteControl
// kReadToolDefaultBytesCap / kReadToolMaxBytesCeiling). Duplicated here
// as Core-only constants so this TU does not depend on remotecontrol.h.
constexpr int kDefaultBytesCap = 512 * 1024;       // 512 KiB
constexpr int kMaxBytesCeiling = 4 * 1024 * 1024;  // 4 MiB
constexpr int kMaxTail         = 10000;

struct Options {
    QString include;            // QRegularExpression; empty = no filter
    QString exclude;            // QRegularExpression; empty = no filter
    QString contains;           // literal substring; empty = no filter
    QString since;              // lexical lower bound on the [..] prefix
    int     tail = 0;           // <=0 = no tail; clamped to kMaxTail
    int     maxBytes = 0;       // <=0 = kDefaultBytesCap; clamped to ceiling
    bool    hasSinceCursor = false;
    QString sinceCursor;        // opaque byte-offset token (decimal)
};

// Filter the already-resolved file at `path`. Returns the read_log
// response envelope:
//   {ok:true, path, lines[], matched, scanned, returned, truncated,
//    lines_dropped?, bytes_cap_clamped?, cursor,
//    cursor_stale?, stale_reason?}
// or a refusal {ok:false, code, error}. Refusal codes:
//   not_found  — file cannot be opened.
//   bad_args   — include/exclude is not a valid QRegularExpression.
// A bad/rotated since_cursor is NOT a refusal: it soft-falls-back to a
// full re-read with cursor_stale:true (mirrors get_scrollback's posture).
QJsonObject filter(const QString &path, const Options &opts);

}  // namespace ReadLog
