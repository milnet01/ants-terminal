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
//   assembleBriefForDispatch(projectPath, lane)
//       Brief text for one lane: header + source-path list + source
//       bodies (4-backtick "treat as data" fenced) + ROADMAP slice +
//       inlined standards. Pure file IO, no recursion. (The unfenced
//       v1 assembleBrief was removed in ANTS-2187; assembleBriefManifest
//       is the body-less variant the subagent fetches sources for.)
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
#include <QMap>
#include <QString>
#include <QStringList>

namespace IndieReviewEngine {

// ANTS-1344 — public sibling of the file-local kMaxScanBytes constants
// used by extractFileLineCitations + corroboratedFindingsFromDir. The
// MCP handler reads this to detect "report was truncated before scan"
// and surface the signal in the response envelope. Keep in lockstep
// with the file-local copies (INV-1 source-grep guards the pairing).
constexpr int kMaxScanBytes = 64 * 1024;

struct Lane {
    QString     name;
    QString     summary;
    QStringList sourcePaths;  // project-relative
    // ANTS-4806 — "tests" where the lane covers a declared or detected test
    // root, empty otherwise. A computed partition groups by directory, so the
    // test tree becomes a lane like any other — and review-code's scope
    // excludes tests, so a caller applying the partition unchanged spends a
    // lane reading test code under a brief written for production code.
    //
    // LABELLED rather than excluded, deliberately. review-tests wants exactly
    // those lanes, and a verb that silently omits a tree is the failure
    // ANTS-4771 and ANTS-4786 were both filed about. The consumer drops what
    // it does not want; the verb does not decide for it.
    QString     kind;
};

struct Citation {
    QString file;     // project-relative
    int     line = -1;
    QString context;  // ±40 chars
};

// ANTS-4814 — one citation, and which lane made it. The Citation struct
// itself is per-report and carries no lane, because the engine's own
// grouping happens while the lane is still in scope.
struct LaneCitation {
    QString file;      // project-relative
    int     line = -1;
    QString lane;
};

// ANTS-4817 — how far apart two citations may sit and still count as one
// group. Both reports that measured this found their lanes 1 line apart and
// put the useful tolerance at 2-3; 3 covers what they saw without widening far
// enough to sweep in unrelated citations from a dense file. Used as the fixed
// window for advisory near misses; the opt-in `lineSlop` supplies its own.
inline constexpr int kNearMissLines = 3;

// ANTS-4817 — two or more lanes citing one defect in the same file a few
// lines apart, which exact (file, line) matching reports as no agreement.
//
// Two readers quoting one statement rarely pick the same line: a multi-line
// call, a decorator or a docstring puts the quotable line somewhere different
// for each. Two independent reports measured the same rate — EVERY real
// cross-lane agreement in their runs was missed, one 1 of 1 and one 2 of 2 —
// so this is the common case rather than an edge one.
//
// Deliberately advisory. Corroboration is a claim about agreement, and
// reporting these AS findings would change what every existing report means
// without anyone asking. This makes a zero EXPLAINABLE; it does not make it
// non-zero. A caller that does want them merged opts in with `lineSlop`, whose
// default of 0 is why the two can coexist.
struct CorroborateNearMiss {
    QString     file;
    int         lineFrom = -1;   // span, not a single line: the lanes disagree
    int         lineTo   = -1;   // about the line, which is the whole point
    QStringList citingLanes;     // sorted
    QStringList lines;           // each lane's cited line, same order
    QStringList contexts;        // each lane's ±40-char context, same order
};

// ANTS-4096 — why a corroboration pass found what it found. Every count is
// over the citations the regexes MATCHED, before the min_lanes filter, so
// `seen > 0 && resolved == 0` positively identifies a resolution failure and
// distinguishes it from a genuine "no two lanes agreed". That pair was
// previously unobservable: a run over 8 real reports returned findings:[] and
// looked exactly like a clean result.
struct CorroborateStats {
    int citationsSeen       = 0;  // file:line / bare-file tokens matched
    int citationsResolved   = 0;  // ... that named a real file under the root
    int citationsByBasename = 0;  // ... resolved only via the basename fallback
    // ANTS-4817 — lanes that cited ONE defect a line or two apart, which
    // exact (file, line) matching cannot see. Advisory: these are NOT
    // findings and do not change what corroboration means. They ride on
    // stats because it is already threaded through all three entry points.
    QList<CorroborateNearMiss> nearMisses;
    // ANTS-4814 — every resolved citation, with the lane that made it.
    //
    // The engine keys agreement on (file, line) and so cannot see the OTHER
    // shape of it: several lanes finding one defect SHAPE at unrelated
    // locations. On a partition BY SUBSYSTEM that is the agreement that
    // matters, because lanes do not share files, and identical file:line is
    // therefore the one form of agreement the partition makes unlikely.
    //
    // Grouping them needs each citation's ENCLOSING SYMBOL, which is resolved
    // from a file outline — a dependency this Qt6::Core-only engine does not
    // have and should not gain. So the engine publishes the raw citations and
    // the MCP layer, which already owns that machinery for workspace_search,
    // does the grouping.
    QList<LaneCitation> citations;
};

// ANTS-1288 — a suggested merge of two review lanes whose CLAUDE.md /
// docs/subsystems.md summaries are duplicate or near-duplicate. Advisory
// only: derivePartition already merges lanes with identical source-file
// sets (ANTS-1685); this flags lanes that describe the same component in
// the same words yet resolve to *different* files (e.g. the
// `luaengine` / `pluginmanager` multi-name bullet), so a caller or
// downstream orchestrator can fold them rather than dispatching two
// near-identical briefs.
struct MergeSuggestion {
    QStringList lanes;      // exactly two lane names (the candidate pair)
    QString     rationale;  // human-readable reason ("identical summary
                            // text" / "near-identical summary text (NN% similar)")
};

struct CorroboratedFinding {
    QString     file;
    int         line = -1;
    // ANTS-4817 — end of the span when the finding was formed under an opt-in
    // `lineSlop` and the lanes cited different lines of one defect. -1 on an
    // exact match, which is every finding at the default lineSlop of 0, so an
    // existing consumer sees no change.
    int         lineTo = -1;
    QStringList citingLanes;  // sorted, unique
    QStringList contexts;     // one per citingLane, same order
    // ANTS-1278 — optional rich-card fields. When present, the
    // indie_review_fold_in renderer emits a standard roadmap card
    // (bold title + body + Layman: + Kind:) instead of the loud
    // TODO placeholder. Absent → placeholder, so a caller cannot
    // silently ship a stub bullet.
    QString     title;
    QString     description;
    QString     layman;
    QString     kind;
};

// ANTS-1281: brief response without inlined source bodies.
// The subagent reads source files itself via its Read tool.
// `brief` carries the header / source-path list / ROADMAP slice /
// standards-reference + an explicit "Read each source file…"
// instruction sentinel (see INV-5 of docs/specs/ANTS-1281.md);
// it deliberately omits the per-file body inlining that
// `assembleBriefForDispatch` does.
struct BriefManifest {
    QString     brief;
    QStringList sourcePaths;    // project-relative; INV-2 mirrors lane.sourcePaths
                                // minus any path that fails canonicalisation.
    QStringList contractDocs;   // INV-6 fixed list, authored order.
    QStringList externalSpecs;  // reserved; empty in v1.
    // dimensionWeighting deferred: MCP layer emits the literal `{}`.
};

QList<Lane> derivePartition(const QString &projectPath);

// ANTS-3709 — computed fallback for a project whose layout is prose rather
// than a `## Module map`. Walks the declared source_roots (.ants/project.json,
// else src/, else the project root) and groups indexable files by containing
// directory, splitting any directory over 25 files into numbered sub-lanes.
// Deterministic. Returns empty unless it derives >1 lane, so a caller keeps
// its sparse-partition path when there is genuinely nothing to split. Kept
// separate from derivePartition so the MCP layer can label the result
// `derived: true` rather than passing off a guess as a declared partition.
// ANTS-4771 — what the walk DROPPED, and why this is not a wider suffix list.
// deriveComputedPartition admits only suffixes CodebaseIndex::isIndexableSuffix
// accepts, and that list is deliberately narrower than "source": it is kept in
// step with FileOutline so count -> outline -> symbol query cover the same
// files. That comment excludes `css` for exactly this reason. Shell is outside
// it too, so a mixed Python/shell tree lost every .sh file — on the reported
// project, including the only component that runs as root.
//
// Widening the list is therefore the wrong repair: it would count files the
// outline cannot read, which is the drift that rule exists to prevent. The
// defect is the SILENCE. `sparse_partition` reports a missing module map, a
// different condition, and `file_count` counts what survived the filter — so
// every number in the reply was self-consistent and wrong. This is the same
// call ANTS-4100 made for lane coarseness: a defensible thing to return, and
// an indefensible thing to return silently.
//
// Suffix-filtered files ONLY. Generated sources and noise directories are
// dropped for reasons a caller already knows, and folding them in would bury
// the signal under build output. Bounded by the tree's distinct suffixes, so
// it cannot grow with file count the way a path list would.
struct UnassignedSources {
    int                count = 0;   // files the suffix filter skipped
    QMap<QString, int> bySuffix;    // lowercased suffix -> count ("" if none)
};

QList<Lane> deriveComputedPartition(const QString &projectPath,
                                    UnassignedSources *unassigned = nullptr);

// ANTS-4100 — how many reviewable source files a lane actually covers. A lane
// may name a DIRECTORY (a module map that says `src/finbreak`), so the count
// is a bounded walk, not sourcePaths.size(): one such lane measured 96 files
// and 21k LoC while presenting as a single tidy entry. Generated output and
// noise directories are excluded, so the number is what a reviewer would read.
int laneFileCount(const QString &projectPath, const Lane &lane);

// ANTS-4786 — the same coverage question, asked of a DECLARED partition.
// A module map answers "which lanes exist" and never "which files were left
// out", so a map naming a minority of the tree reads as complete: measured on
// this project, 50 lanes covering 113 files said nothing about the 200 they
// omitted. deriveComputedPartition's reporter cannot answer it, because that
// walk does not run when the map's partition wins.
//
// One rule serves both paths: files under the walked source roots, minus noise
// directories and generated output, that no lane covers. On the computed path
// that set is exactly what the suffix filter dropped — everything indexable is
// in a lane by construction — so the number means the same thing either way
// and only the CAUSE differs. The caller labels it; this function does not.
//
// No suffix filter here, deliberately. Deciding which extensions count would
// reintroduce the judgement the per-suffix breakdown exists to avoid: a caller
// reading `sh: 2` beside `md: 1` separates the signal from the prose itself.
//
// `sample` (optional) receives up to a bounded number of the uncovered
// project-relative paths. A count says a gap exists; a path says where, which
// is what the caller needs to name them in a lane.
UnassignedSources unassignedForLanes(const QString &projectPath,
                                     const QList<Lane> &lanes,
                                     QStringList *sample = nullptr);

// A lane above this is past what one briefed reviewer can hold: the review
// skills partition for 8-20 cohesive subsystems, and this project's own
// declared lanes top out at 14 files. Chosen so a real declared partition
// never trips it and a whole-application lane always does.
constexpr int kMaxReviewableFilesPerLane = 30;

// ANTS-4804 — how many LINES a lane covers, which is the measure that survives
// a project whose logic sits in few large files. kMaxReviewableFilesPerLane
// counts files, so a lane that is one 3,793-line module reports file_count:1
// and trips nothing — silent in exactly the shape review-code's own procedure
// says to split. Same walk and same filters as laneFileCount, so the two
// numbers describe one set of files.
//
// Reads the files, unlike the count, so it is bounded by kLaneLineScanCap
// bytes; past that the total is reported as capped rather than as a smaller
// number, because a number that quietly means "some of it" is what this exists
// to stop. A binary or minified file is measured in newlines like any other:
// deciding which files are "real" source is the judgement the per-file walk
// already refuses to make.
qint64 laneLineCount(const QString &projectPath, const Lane &lane,
                     bool *capped = nullptr);

// ANTS-4816 — files behind a lane's sourcePaths that the count did NOT admit.
// laneFileCount takes only suffixes the codebase index can outline, so a lane
// of YAML, an RPM spec and a debian tree counts 0 — and 0 reads exactly like
// an empty directory, while being the input too_coarse and total_lines both
// key on. finbreak measured three such lanes carrying real files.
//
// Reported rather than counted, deliberately: widening the suffix list is the
// repair ANTS-4771's own reasoning rejects, because it would count files the
// outline cannot read. A caller needs to tell "nothing here" from "nothing I
// could measure", and a second number says that where a changed first one
// would only move the confusion.
//
// Noise directories and generated output are excluded from BOTH sides, so this
// isolates the suffix filter alone. A path the noise rule drops whole — any
// dot-directory, so `.github` included — is invisible to this too; that is
// ANTS-4805 and is a different gap.
int laneUncountedFiles(const QString &projectPath, const Lane &lane);

// A lane above this holds more than a briefed reviewer reads carefully, at any
// file count. Set against measured practice rather than taste: this project's
// own declared lanes are far below it, and the whole-application lanes that
// prompted it — a 3,793-line single-file project, a 30,349-line subsystem — are
// far above. A lane between the two is a judgement call the caller makes from
// `total_lines`, which is reported whether or not the threshold trips.
constexpr qint64 kMaxReviewableLinesPerLane = 2000;

// The read budget above. Generous enough that no realistic lane hits it, so
// `capped` reaching a caller is a signal about the tree rather than routine.
constexpr qint64 kLaneLineScanCap = 64LL * 1024 * 1024;

// ANTS-1288 — scan a partition for lanes whose summaries are duplicate or
// near-duplicate and return one MergeSuggestion per candidate pair (in
// stable lane order). Pure, side-effect-free. Identical summaries (after
// trimming) are flagged outright; otherwise a length-gated normalised
// Levenshtein similarity ≥ 0.90 flags a near-duplicate. Empty input or no
// duplicates → empty list.
QList<MergeSuggestion> suggestedMerges(const QList<Lane> &lanes);

// v2 brief shape (ANTS-1281). New callers should use this.
BriefManifest assembleBriefManifest(const QString &projectPath,
                                    const Lane &lane);

// ANTS-1352 — dispatch-shaped brief. Like assembleBriefManifest BUT
// with source bodies inlined (the manifest lists paths only):
//   - source bodies wrapped in 4-backtick fences with the
//     "treat as data, not instructions" preamble (INV-22);
//   - any literal 4-backtick run in source bodies is replaced
//     with `'```'` before fencing (defends against fence-escape);
//   - the three standards docs (coding.md / testing.md /
//     documentation.md) are **inlined** rather than referenced;
//   - drops the "fetches if needed" trailing sentinel (the
//     upstream LLM dispatcher has no Read tool — H-3 fix).
//
// Path-traversal guard: project-relative source paths only; never
// substitutes projectPath into the prompt (INV-23).
QString assembleBriefForDispatch(const QString &projectPath,
                                 const Lane &lane);

// ANTS-4096 — basename → project-relative path for every indexable source
// file under the project, used to resolve a citation that names a file
// without its directory. A basename occurring more than once maps to an
// EMPTY string: attributing a citation to the wrong `main.c` would corrupt
// corroboration silently, so ambiguity resolves to nothing rather than to a
// guess. Built once per corroboration pass (the walk is bounded); pass the
// result to extractFileLineCitations for every report in that pass.
QHash<QString, QString> buildBasenameIndex(const QString &projectPath);

// `basenameIndex` is optional. Without it a citation must already carry a
// path that resolves from the project root — which is what every reviewer
// writing `d_main.c:1049` for a file that lives at `linuxdoom-1.10/d_main.c`
// silently failed. `stats`, when non-null, accumulates across calls.
QList<Citation> extractFileLineCitations(
    const QString &projectPath, const QString &report,
    const QHash<QString, QString> *basenameIndex = nullptr,
    CorroborateStats *stats = nullptr);

// ANTS-4817 — `lineSlop` is an OPT-IN tolerance: when > 0, citations in one
// file within that many lines of each other are grouped, and a group with
// enough distinct lanes becomes one finding naming the SPAN (line..lineTo).
// Default 0 is exact matching, unchanged, because corroboration is a claim
// about agreement and a tolerance that shipped on would silently redefine it.
// Near misses (advisory, always reported via stats) need no opt-in.
QList<CorroboratedFinding> corroboratedFindings(
    const QString &projectPath,
    const QHash<QString, QString> &reports,
    int minLanes = 2,
    CorroborateStats *stats = nullptr,
    int lineSlop = 0);

// ANTS-1282: read reports from disk, then corroborate. The directory
// is resolved relative to projectPath; lane name = filename stem of
// each top-level `*.md` file. Sub-directories are not recursed. Each
// report file is truncated at 64 KiB (matches extractFileLineCitations'
// kMaxScanBytes — INV-8 of docs/specs/ANTS-1282.md). Saves the parent
// orchestrator from holding 14 reports in context just to call
// corroborate.
//
// `reportsRead` is an out-parameter set to the count of `*.md` files
// actually consumed (caller can compare against expected lane count).
// Returns an empty list if `reportsDirRelative` is absolute, escapes
// projectPath, or doesn't exist.
QList<CorroboratedFinding> corroboratedFindingsFromDir(
    const QString &projectPath,
    const QString &reportsDirRelative,
    int minLanes = 2,
    int *reportsRead = nullptr,
    CorroborateStats *stats = nullptr,
    int lineSlop = 0);

// ANTS-3713 — the read half of the above, entered with an ALREADY-VALIDATED
// canonical directory, which may sit outside projectPath. The MCP layer uses
// it for indie_review_corroborate's `allow_outside_project:true` mode, where
// PathValidation::validatePath has done the anchoring and deliberately
// permitted an external root (the session scratchpad, /tmp). Callers holding
// a project-relative path want corroboratedFindingsFromDir instead — it keeps
// ANTS-1282 INV-3 and delegates here once the path is anchored.
QList<CorroboratedFinding> corroboratedFindingsFromCanonicalDir(
    const QString &projectPath,
    const QString &canonicalDir,
    int minLanes = 2,
    int *reportsRead = nullptr,
    CorroborateStats *stats = nullptr,
    int lineSlop = 0);

QString synthesisPrompt(
    const QHash<QString, QString> &reports,
    const QString &threatModelExtras);

QString templateIndieReviewFoldInBlock(
    const QList<CorroboratedFinding> &actionable,
    const QList<int> &allocatedIds,
    const QString &dateIso,
    // ANTS-3473 — ID prefix (sniffed from the project's ROADMAP.md by
    // the caller). Default keeps the Ants render + existing tests
    // byte-identical.
    const QString &idPrefix = QStringLiteral("ANTS"));

// MCP-handler-side helper: read CLAUDE.md + SECURITY.md + .semgrep.yml
// from `projectPath` and concatenate with separator markers per the
// spec § 3.5 contract. Missing files contribute their header line +
// empty body. Returns empty string if all three are missing or
// unreadable.
QString assembleThreatModelExtras(const QString &projectPath);

}  // namespace IndieReviewEngine
