// ANTS-2021 — read_region MCP tool: pure region/symbol slicing helper.
// Qt6::Core-only, lives in ants_core_lib so it is unit-testable without
// RemoteControl / MainWindow (mirrors readlog.h / fileoutline.h). The thin
// cmdReadRegion wrapper (remotecontrol.cpp) handles path resolution +
// PathValidation + the caller_cwd contract, then calls ReadRegion::extract
// on the resolved path. See docs/specs/ANTS-2021.md.

#pragma once

#include <QJsonObject>
#include <QString>

namespace ReadRegion {

// Read-tool family caps, duplicated as Core-only constants so this TU does
// not depend on remotecontrol.h (mirrors readlog.h).
constexpr int kDefaultBytesCap = 512 * 1024;       // 512 KiB
constexpr int kMaxBytesCeiling = 4 * 1024 * 1024;  // 4 MiB

struct Options {
    bool    hasLine   = false;  // line-range mode selected
    int     startLine = 0;      // 1-based inclusive
    int     endLine   = 0;      // 1-based inclusive
    QString symbol;             // non-empty → symbol-body mode
    int     maxBytes  = 0;      // <=0 → kDefaultBytesCap; clamped to ceiling
};

// Slice the already-resolved file at `absPath`. Returns the read_region
// response envelope:
//   {ok:true, path, start_line, end_line, lines[], returned, truncated,
//    bytes_cap_clamped?, symbol?, symbol_ambiguous?, symbol_match_count?}
// or a refusal {ok:false, code, error}. Refusal codes:
//   bad_args         — neither/both selectors, or start<1 / end<start.
//   not_found        — file cannot be opened.
//   symbol_not_found — symbol mode and no outline symbol matches `symbol`.
// The dispatcher injects `etag` (ANTS-1499); the helper emits none.
QJsonObject extract(const QString &absPath, const Options &opts);

}  // namespace ReadRegion
