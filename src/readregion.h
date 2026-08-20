// ANTS-2021 — read_region MCP tool: pure region/symbol slicing helper.
// Qt6::Core-only, lives in ants_core_lib so it is unit-testable without
// RemoteControl / MainWindow (mirrors readlog.h / fileoutline.h). The thin
// cmdReadRegion wrapper (remotecontrol.cpp) handles path resolution +
// PathValidation + the caller_cwd contract, then calls ReadRegion::extract
// on the resolved path. See docs/specs/ANTS-2021.md.

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

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
    QString section;            // ANTS-2221 — non-empty → markdown section-body mode
    int     maxBytes  = 0;      // <=0 → kDefaultBytesCap; clamped to ceiling
    bool    callSequence = false;  // ANTS-2157 — also emit the ordered call list + accessors
};

// Slice the already-resolved file at `absPath`. Returns the read_region
// response envelope:
//   {ok:true, path, start_line, end_line, lines[], returned, truncated,
//    bytes_cap_clamped?, symbol?, symbol_ambiguous?, symbol_match_count?,
//    section?, section_slug?}
// or a refusal {ok:false, code, error}. Refusal codes:
//   bad_args          — zero/multiple selectors, or start<1 / end<start.
//   not_found         — file cannot be opened.
//   symbol_not_found  — symbol mode and no outline symbol matches `symbol`.
//   section_not_found — section mode and no markdown heading slugifies to
//                       the requested `section` (ANTS-2221).
// Exactly one of {line range, symbol, section} must be selected.
// The dispatcher injects `etag` (ANTS-1499); the helper emits none.
QJsonObject extract(const QString &absPath, const Options &opts);

// ANTS-2219 — read_regions: batched multi-selector read. Resolves each
// `itemsValue[]` entry ({path, symbol|start_line/end_line|section,
// etag_match?}) under `rootCanonical` via PathValidation, runs extract(),
// reframes the path project-relative, and attaches a per-item etag (an item
// whose `etag_match` matches 304s to an {unchanged:true} stub). One shared
// `maxBytes` budget (<=0 → kDefaultBytesCap; clamped to ceiling) is consumed
// in item order. Returns {ok:true, results[], count, truncated?} or a refusal
// {ok:false, code, error}:
//   bad_args        — itemsValue is not an array, or is empty.
//   too_many_items  — more than 64 items.
// Per-item failures (bad/missing path, invalid selector) land in that item's
// result with ok:false; the batch envelope stays ok:true. Pure (Qt6::Core +
// PathValidation), so the handler in remotecontrol.cpp only resolves the root.
// ANTS-3589 — `defaultPath` is the top-level `path` fallback: any item that
// omits its own `path` reads from `defaultPath` instead (a per-item `path`
// still wins), so the common "N slices of ONE file" case can pass the filename
// once instead of repeating it on every item. Empty → items must each carry a
// `path` (prior behaviour).
QJsonObject extractBatch(const QString &rootCanonical,
                         const QJsonValue &itemsValue, int maxBytes,
                         const QString &defaultPath = QString());

// ANTS-4350 — rank a document's headings against a query slug that matched
// nothing, so a refusal carries near-misses instead of a dead end. Non-numeric
// word overlap is the primary key and a shared leading numeric token the
// tiebreak: a caller who half-remembers a title gets the WORDS right and the
// number wrong, while one working from a cross-reference ("commits.md § 1.1")
// gets the NUMBER right and the words wrong. Nothing scoring falls back to the
// supplied order. Capped at 10 so a large document does not pay a big refusal.
//
// ANTS-4556 — takes a plain slug list rather than parsed headings, because
// roadmap_log's `bad_section` ranks SECTION slugs it already holds and there
// is no document to re-scan. Second caller, so the shape is shared rather than
// copied.
QStringList rankSectionCandidates(const QString &wantSlug,
                                  const QStringList &slugs);

}  // namespace ReadRegion
