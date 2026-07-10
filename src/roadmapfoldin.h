// RoadmapFoldIn — atomic ROADMAP.md fold-in helper for ANTS-1111 (and
// ANTS-1112 / 1113 reuse). Qt::Core only, no widgets.
//
// Two operations, both atomic / advisory-locked:
//
//   allocateIds(projectPath, n)
//       Grab N consecutive IDs from `<projectPath>/.roadmap-counter`
//       under ::flock(LOCK_EX|LOCK_NB) — adopts the same advisory-
//       lock pattern as ConfigWriteLock in configbackup.h:82-117.
//       Atomic via QSaveFile — even a kill-9 between read and write
//       leaves the counter file intact.
//
//   insertBlock(projectPath, releaseBlockHeading, block)
//       Insert `block` (a fully-formed `### …` fold-in subsection per
//       roadmap-format.md § 3.8) into <projectPath>/ROADMAP.md on the
//       line immediately following the line whose text exactly equals
//       `releaseBlockHeading`. Returns false if the heading isn't
//       found OR any IO fails. ROADMAP.md's existing permissions are
//       preserved (typically 0644 — it's a checked-in source file).
//
//   findActiveReleaseHeading(projectPath)
//       Locate the "active" release block in <projectPath>/ROADMAP.md:
//       first `^## ` heading whose body matches `\(target:\s*\d{4}-\d+\)`
//       (in-flight); otherwise the first `^## ` heading whose body
//       matches `^\d+\.\d+\.\d+\s*—` (most-recent shipped). Empty
//       string if neither matches. Returns the FULL heading line so
//       the caller can pass it directly to insertBlock.

#pragma once

#include <QList>
#include <QString>

namespace RoadmapFoldIn {

// Allocate `n` consecutive IDs from <projectPath>/.roadmap-counter.
// Returns the list of allocated integers (e.g. {1257, 1258, 1259})
// on success; empty list on flock failure (5 s budget) or any IO
// error. ANTS-1490: if flock returns ENOLCK / EBADF / EINVAL (typical
// on networked or some FUSE filesystems), falls back to O_CREAT|O_EXCL
// rename-based locking on a `.roadmap-counter.lock` sibling.
QList<int> allocateIds(const QString &projectPath, int n);

// ANTS-2227 — non-mutating peek of the next N ids allocateIds WOULD return,
// WITHOUT bumping .roadmap-counter. For fold-in dry_run previews: same id math
// as allocateIds (current+1 … current+N) via inspectCounter, and an empty list
// on any counter error — so a caller reuses its allocateIds "counter_failed"
// refusal path verbatim (`dryRun ? peekIds : allocateIds`).
QList<int> peekIds(const QString &projectPath, int n);

// ANTS-1490 — counter-file path resolver, exposed so callers can
// include it in their error envelopes when allocateIds returns empty.
// Returns the absolute path (canonicalised projectPath +
// "/.roadmap-counter"); empty when projectPath fails to canonicalise.
QString counterFilePath(const QString &projectPath);

// ANTS-3450 — the project's true ID high-water mark, derived from the
// committed roadmap corpus rather than the `.roadmap-counter` cache.
// Scans, for `prefix`-NNNN ids, the three committed files that between
// them retain every allocated id even after bullets migrate out of
// ROADMAP.md:
//   • ROADMAP.md          — live bullets
//   • CHANGELOG.md        — shipped items folded out of the roadmap
//   • docs/roadmap/*.md   — whole minors rotated out (rotate-roadmap.sh)
// This lets `.roadmap-counter` be a pure, untracked local cache:
// allocation floors to this value, so a stale/absent/wiped counter can
// never reissue a live id. When `prefix` is empty the project's dominant
// counter-style prefix is sniffed from ROADMAP.md; returns 0 when nothing
// matches (a truly greenfield roadmap). projectPath is canonicalised;
// missing corpus files are skipped. Read-only.
qint64 corpusHighWater(const QString &projectPath,
                       const QString &prefix = {});

// ANTS-3473 — the project's dominant counter-style ID prefix, sniffed
// from <projectPath>/ROADMAP.md (the most frequent `[PREFIX-NNNN]`
// bracketed token, e.g. "FIBR" for a finbreak roadmap; dominance-by-count
// shrugs off strays like "[UTF-8]"). The fold-in ID renderers use this so
// a fold-in into a non-Ants project stamps that project's prefix instead
// of a hardcoded "ANTS". Returns `fallback` when ROADMAP.md is
// absent/unreadable or carries no counter-style id. Read-only.
QString sniffIdPrefix(const QString &projectPath,
                      const QString &fallback = QStringLiteral("ANTS"));

// ANTS-3480 — canonical fold-in ID renderer. Zero-pads the numeric suffix
// to a minimum of 4 digits, exactly matching roadmap_log op:append's render
// (remotecontrol.cpp cmdRoadmapLogAppend: `.arg(n, 4, 10, QLatin1Char('0'))`),
// so a fold-in bullet's `[PREFIX-NNNN]` token is byte-identical in width to
// every hand- and append-written bullet and therefore sorts / regex-matches
// (`/PREFIX-\d{4}/`) the same. Used by all three fold-in block renderers AND
// their `allocated_ids` echo, so the id a caller reads back equals the id
// written into the block. A suffix already ≥4 digits is emitted verbatim.
QString renderId(const QString &prefix, int n);

// ANTS-1618 — post-failure inspection helper. allocateIds returns an
// empty QList on every failure mode (flock contention, corrupt int,
// permission, symlink-escape); callers want a state-specific message.
// Call this AFTER a failed allocate (the lock is already released).
// Read-only; opens the counter file unlocked for inspection.
enum class CounterState {
    Ok,                // file contains a valid integer
    EmptyOrAbsent,     // file doesn't exist OR is empty/whitespace
                       // (NOTE: allocateIds now auto-inits this case)
    Corrupt,           // non-integer content
    PermissionDenied,  // cannot open for read
    PathRefused,       // counterStaysInProject() guard failed
};

struct CounterInspection {
    CounterState state = CounterState::Ok;
    QString      preview;  // first ~80 bytes when state==Corrupt
    int          value = 0; // parsed integer when state==Ok
};

CounterInspection inspectCounter(const QString &projectPath);

// Atomic insert per the doc-comment above. Returns true on success,
// false on heading-not-found or IO error.
bool insertBlock(const QString &projectPath,
                 const QString &releaseBlockHeading,
                 const QString &block);

// Active-release heading lookup per the doc-comment above. Empty
// string when no recognisable release block exists.
QString findActiveReleaseHeading(const QString &projectPath);

} // namespace RoadmapFoldIn
