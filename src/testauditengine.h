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
// v2 follow-ups (logged separately):
//   * Project-internal grep-patterns JSON resource.
//   * Drift-guard test linking kDimensions to references/dimensions.md.
//   * mtime-walk recursive scan + inotify (current shallow recheck
//     has documented limitation; INV-15).
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
// human prose, kept in sync manually.
const QStringList &kDimensions();

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
