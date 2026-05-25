// ANTS-1397 — `test_audit_*` MCP trio engine (skill-displacement).
//
// Four-verb trio mirrors cold_eyes / indie_review / debt_sweep:
//   partition → brief → synthesis_prompt → fold_in
//
// v1 scope (this implementation):
//   * Framework detection (pytest / jest / ctest / cargo / go).
//   * File-tree walk + glob match.
//   * Hardcoded pre-pass pattern set (no project-internal JSON yet;
//     v2 ships docs/standards/test-audit-grep-patterns.json).
//   * Chunk packing depth-first, size [4, 30] (default 12).
//   * partition_token via qHash (NOT SHA-256 — staleness only, per
//     user decision 2026-05-17). Bounded shallow recheck in brief
//     and synth with 5 s rate-limit (INV-15).
//   * Brief returns structured fields ONLY (no `brief` string;
//     caller composes from structured siblings).
//   * Synthesis reads `<project>/<reports_dir>/*.md`; each report
//     fenced via `<chunk_report file="…">…</chunk_report>` to defend
//     against prompt injection (INV-8).
//   * fold_in delegates to RoadmapFoldIn::* engine entries directly
//     (NOT MCP re-entry); single batched insertBlock per call (INV-3).
//   * pre_pass_findings carry {file, line, pattern_id, dimension}
//     ONLY — no matched text (INV-13).
//   * Pagination per ANTS-1436 on chunks[].
//
// v2 status (ANTS-1450):
//   * SHIPPED: in-tree source-of-truth resource
//     docs/standards/test-audit-grep-patterns.json + drift-guard test
//     (tests/features/test_audit_pattern_drift/) asserting kDimensions()
//     and prePassPatterns() match it exactly.
//   * DEFERRED (ANTS-1706): mtime-walk recursive scan + inotify (current
//     shallow recheck has documented limitation; INV-15), pre_pass token
//     recompute in brief/synth, and envelope byte-cap cascade for
//     pre_pass_findings on large suites.
//
// See docs/specs/ANTS-1397.md.

#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace TestAuditEngine {

// 18 dimensions — engine canonical (INV-6). Skill markdown
// (~/.claude/skills/test-audit/references/dimensions.md) is the
// human prose, kept in sync manually; the in-tree source of truth is
// docs/standards/test-audit-grep-patterns.json (ANTS-1450), against
// which a drift-guard test asserts this list.
const QStringList &kDimensions();

// Pre-pass grep pattern set (INV-7). Each entry's `dimension` is one of
// kDimensions(). Mirrored by docs/standards/test-audit-grep-patterns.json
// (ANTS-1450) — a drift-guard test asserts this table and the JSON match
// (same ids, dimensions, regexes, in order). Exposed so that guard can
// run without reaching into engine internals.
struct PrePassPattern {
    QString id;
    QString dimension;
    QString regex;          // PCRE2
};
const QVector<PrePassPattern> &prePassPatterns();

struct Chunk {
    QString     id;            // "c-001", "c-002", ...
    QStringList paths;
    // ANTS-1487: renamed from dimensionHints — accurate, since this is
    // "dimensions the cheap pre-pass grep hit" not "the only dimensions
    // worth auditing". Callers should still seed the chunk subagent
    // with `dimensions_active` (the full lane list) and only use this
    // for prioritisation.
    QStringList prePassDimensions;
};

struct PartitionRequest {
    QString callerCwd;
    QString scope;             // "auto" | "path:<sub>" | "files:<csv>"
    QString dimensions;        // "auto" | "csv:<d1,d2,...>"
    int     chunkSize = 12;    // clamped [4, 30]
    bool    quick = false;
    int     offset = 0;
    int     limit = -1;        // -1 → no caller limit
};

struct PartitionResult {
    bool                              ok = true;
    QString                           error;
    QString                           code;
    QString                           framework;
    // ANTS-1623 — polyglot framework signals. Other frameworks whose
    // signal files were detected at the probed root (or, when scope
    // is "path:<sub>", at top-level subdirs of the project) but were
    // not picked as the primary `framework`. Each entry is a
    // {name, root_rel} pair. Callers can fan out a second partition
    // call with `scope: "path:<root_rel>"` to cover the extra tree
    // without re-walking from project root. Empty in single-framework
    // projects (the common case).
    QJsonArray                        additionalFrameworks;
    QStringList                       testGlobs;
    QVector<Chunk>                    chunks;
    int                               totalFiles = 0;
    int                               chunksCount = 0;
    QStringList                       dimensionsActive;
    QStringList                       dimensionsSkipped;
    QHash<QString, QString>           skipReasonPerDimension;
    QHash<QString, QJsonArray>        prePassFindingsByChunk;
    bool                              prePassCached = false;
    QString                           partitionToken;   // INV-4
    int                               offset = 0;
    int                               limit = -1;
    int                               total = 0;
    bool                              truncated = false;
    int                               nextOffset = -1;
    int                               byteCount = 0;
    QDateTime                         mtimeWalkComputedAt;  // INV-15
};

struct BriefRequest {
    QString callerCwd;
    QString chunkId;
    QString partitionToken;
};

struct BriefResult {
    bool        ok = true;
    QString     error;
    QString     code;
    QString     chunkId;
    QStringList sourcePaths;
    QStringList dimensions;
    QJsonObject frameworkContext;
    QJsonArray  prePassFindings;
    QJsonArray  priorFalsePositives;  // ANTS-1457
    int         byteCount = 0;
};

struct SynthRequest {
    QString     callerCwd;
    QString     partitionToken;
    QString     reportsDir;              // project-relative OR absolute
    QJsonObject calibrationAnchor;       // optional {raw, actionable, noise_rate_pct}
    bool        allowOutsideProject = false;  // ANTS-1455 — opt-in escape hatch
    QString     mode;                    // ANTS-1455 — "summary" (default) | "full"
    int         offset = 0;              // ANTS-1455 — pagination cursor (full mode)
    int         limit = -1;              // ANTS-1455 — page size; -1=default
};

struct SynthResult {
    bool        ok = true;
    QString     error;
    QString     code;
    QString     prompt;
    QJsonObject dimensionSummaries;
    int         reportsRead = 0;
    int         byteCount = 0;
    // ANTS-1455 — full-mode pagination fields.
    int         chunksTotal = 0;
    int         chunksReturned = 0;
    int         nextOffset = -1;
    bool        truncatedByLimit = false;
    // ANTS-1455 — summary-mode envelope fields.
    QString     mode;                    // echoed back: "summary" | "full" | "hybrid"
    QJsonArray  topDimensions;           // [{dimension, count}]
    QJsonArray  fileIndex;               // [{file, dimension_hits_total}]
    bool        truncated = false;       // either summary cap fired
    // ANTS-1488 — per-dimension severity histograms. Map of
    // dimension → {crit, high, med, low, info} so the orchestrator can
    // tell at a glance whether any chunk surfaced a CRITICAL without
    // re-reading every per-chunk markdown. Parsed from the standard
    // `- [SEV] file:line — …` finding shape under `## <emoji> <Dim> (N)`
    // section headers. Emitted in summary + hybrid modes.
    QJsonObject severityHistograms;
};

struct FoldInRequest {
    QString     callerCwd;
    QJsonArray  actionable;          // [{dimension, severity, file, line, summary, fix}, ...]
    QString     framework;
    int         filesScanned = 0;
    QStringList dimensions;
    int         rawFindings = 0;
    // ANTS-1635 — narrative-mode opt-in. When `narrativeMode==true`,
    // `actionable[]` is no longer required; `narrativeMd` is inserted
    // verbatim under the `### 🧪 Test Audit YYYY-MM-DD` heading instead
    // of the per-finding bullet rendering. ID allocation is skipped
    // entirely (no `.roadmap-counter` touch). The metadata line
    // (Framework / Files scanned / …) is omitted because the caller's
    // narrative carries its own structure. Closes the "I want a prose
    // ROADMAP subsection, not 30 bullets" friction class observed in
    // Music Production /test-audit 2026-05-18.
    bool        narrativeMode = false;
    QString     narrativeMd;
};

struct FoldInResult {
    bool        ok = true;
    QString     error;
    QString     code;
    QString     block;
    QStringList allocatedIds;
    QString     written;
    QString     releaseBlockHeading;
    int         bytesWritten = 0;
    int         writtenCount = 0;
    int         failedCount = 0;
    bool        partial = false;
    // ANTS-1527 — counter-file path populated on id_counter_failed so
    // the caller has a programmatic handle to the file (and the
    // implicit `<path>.lock` sibling) without parsing the prose error
    // message. Empty on success / other failure modes.
    QString     counterPath;
};

PartitionResult partition(const PartitionRequest &req);
BriefResult     brief(const BriefRequest &req);
SynthResult     synthesize(const SynthRequest &req);
FoldInResult    foldIn(const FoldInRequest &req);

// ANTS-1513 — recheck a deferred finding's cite before picking the work
// back up days later. Parses ROADMAP.md for the bullet `[<findingId>]`,
// pulls the first `path:line` cite out of its body, and reports whether
// the file still exists and whether the cited line still trips any
// pre-pass smell pattern. When the file is gone, a best-effort git
// rename hint is offered. Read-only; no project mutation.
struct RecheckRequest {
    QString callerCwd;
    QString findingId;     // e.g. "ANTS-1234" (the roadmap bullet id)
};
struct RecheckResult {
    bool    ok = false;
    QString error;
    QString code;
    bool    found = false;          // bullet `[<findingId>]` located
    QString citedFile;              // path as cited (relative to project)
    int     citedLine = -1;         // 1-based; -1 when no cite parsed
    bool    fileExists = false;
    bool    lineExists = false;     // citedLine within the file's bounds
    QString currentLineText;        // trimmed content now at citedLine
    bool    lineStillMatchesPattern = false;  // any pre-pass regex hits
    QString matchedPatternId;       // first matching pattern's id
    QString matchedDimension;       // ...and its dimension
    // NB: the best-effort git rename `drift_hint` for a missing file is
    // added by the MCP registration lambda (mainwindow.cpp) — the engine
    // stays shell-free / pure (test_audit trio INV-1).
};
RecheckResult   recheck(const RecheckRequest &req);

// In-process partition cache so brief / synth can recover the
// chunk layout for a given token without re-walking the tree.
// Bounded; LRU-evicted to kPartitionCacheCap entries.
namespace internal {
const PartitionResult *lookupPartition(const QString &token);

// Exposed for ANTS-1451 regression coverage. Walks the project tree
// honouring the test_globs, with build-tree + tooling exclusions
// baked in. `scope` accepts "auto", "path:<sub>", "files:<csv>".
QStringList walkTestFiles(const QString &projectRoot,
                          const QStringList &testGlobs,
                          const QString &scope);
}  // namespace internal

}  // namespace TestAuditEngine
