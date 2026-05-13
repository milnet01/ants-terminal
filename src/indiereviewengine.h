// IndieReviewEngine — pure-function helpers for the in-process
// /indie-review fold (ANTS-1112 v1). Qt::Core only, no widgets.
//
// Six operations:
//
//   derivePartition(projectPath)
//       Read CLAUDE.md `## Module map (src/)` via SubsystemMap; for
//       each lane, walk src/ to compute the source-file set
//       (`<name>.{h,cpp}` + `<name>*.{h,cpp}` siblings, generated
//       files skipped). Optional override:
//       <projectPath>/.indie-review/partition.json.
//
//   assembleBrief(projectPath, lane)
//       Verbatim brief text for one lane: header + source bodies +
//       ROADMAP slice + standards trail (links only). Pure file IO,
//       no recursion.
//
//   extractFileLineCitations(projectPath, report)
//       Regex pass over a single review report; returns Citation
//       structs whose path resolves under projectPath. Reports
//       larger than 64 KiB are truncated (defensive, matches
//       MAX_TOOL_OUTPUT_BYTES convention).
//
//   corroboratedFindings(projectPath, reports, minLanes=2)
//       Cross-lane corroboration: every (file, line) cited by
//       >= minLanes distinct lanes. (file, -1) and (file, 42) are
//       distinct keys (intentional, see spec INV-6).
//
//   synthesisPrompt(reports, threatModelExtras)
//       Pure string templating of a prompt for the optional
//       cross-cutting synthesis LLM call. Caller dispatches.
//
//   templateIndieReviewFoldInBlock(actionable, allocatedIds, dateIso)
//       `### 🔍 Indie-review fold-in (DATE)` block with one bullet
//       per finding. Mirrors AuditEngine::templateRoadmapFoldInBlock
//       (ANTS-1111 v1).

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace IndieReviewEngine {

struct Lane {
    QString     name;
    QString     summary;
    QStringList sourcePaths;  // project-relative
};

struct Citation {
    QString file;     // project-relative
    int     line = -1;
    QString context;  // ±40 chars
};

struct CorroboratedFinding {
    QString     file;
    int         line = -1;
    QStringList citingLanes;  // sorted, unique
    QStringList contexts;     // one per citingLane, same order
};

QList<Lane> derivePartition(const QString &projectPath);

QString assembleBrief(const QString &projectPath, const Lane &lane);

QList<Citation> extractFileLineCitations(
    const QString &projectPath, const QString &report);

QList<CorroboratedFinding> corroboratedFindings(
    const QString &projectPath,
    const QHash<QString, QString> &reports,
    int minLanes = 2);

QString synthesisPrompt(
    const QHash<QString, QString> &reports,
    const QString &threatModelExtras);

QString templateIndieReviewFoldInBlock(
    const QList<CorroboratedFinding> &actionable,
    const QList<int> &allocatedIds,
    const QString &dateIso);

// MCP-handler-side helper: read CLAUDE.md + SECURITY.md + .semgrep.yml
// from `projectPath` and concatenate with separator markers per the
// spec § 3.5 contract. Missing files contribute their header line +
// empty body. Returns empty string if all three are missing or
// unreadable.
QString assembleThreatModelExtras(const QString &projectPath);

}  // namespace IndieReviewEngine
