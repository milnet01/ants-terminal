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

// ANTS-1490 — counter-file path resolver, exposed so callers can
// include it in their error envelopes when allocateIds returns empty.
// Returns the absolute path (canonicalised projectPath +
// "/.roadmap-counter"); empty when projectPath fails to canonicalise.
QString counterFilePath(const QString &projectPath);

// Atomic insert per the doc-comment above. Returns true on success,
// false on heading-not-found or IO error.
bool insertBlock(const QString &projectPath,
                 const QString &releaseBlockHeading,
                 const QString &block);

// Active-release heading lookup per the doc-comment above. Empty
// string when no recognisable release block exists.
QString findActiveReleaseHeading(const QString &projectPath);

} // namespace RoadmapFoldIn
