// ANTS-3601 — deterministic doc-integrity engine. Qt6::Core only, FS-reading,
// no widgets, no subprocess (mirrors docsindex.cpp / codebaseindex.cpp). Finds
// three classes of internal-consistency rot in markdown contract docs — all
// computable with no LLM:
//
//   1. Dead anchors  — a `[t](#slug)` / `[t](other.md#slug)` whose slug names
//                      no real heading.
//   2. Broken links  — a `[t](relpath)` whose target file does not exist.
//   3. TOC coverage  — a hand-maintained Table of Contents that omits a
//                      section, or lists a duplicate/dead entry.
//
// Consumed by the `doc_integrity` MCP verb and the cold-eyes Phase-1e feed.
// See docs/specs/ANTS-3601.md.

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace DocIntegrity {

enum class Kind {
    DeadAnchor,   // [t](#slug) / [t](other.md#slug) → slug is no real heading
    BrokenLink,   // [t](relpath) → target file does not exist under root
    TocGap,       // a section missing from the TOC, or a duplicate TOC entry
};

struct Finding {
    Kind    kind;
    QString file;     // project-relative path of the doc the link lives in
    int     line;     // 1-based line of the offending link/entry
    QString message;  // human-readable
};

struct Options {
    qint64 maxDocBytes       = 2 * 1024 * 1024;  // per-doc read budget
    int    maxHeadingsPerDoc = 4000;
    int    maxLinksPerDoc    = 4000;
    int    maxDocsPerRun     = 2000;  // total in-scope docs ceiling; mirrors
                                      // docsindex.h kMaxIndexDocs. Docs beyond
                                      // the cap are skipped, not read.
};

// Check the given project-relative docs. `rootCanonical` anchors relative-link
// and cross-doc-anchor resolution. Findings are returned in (file, line)
// order. Pure: no disk writes, no cache. `checkedDocs` (if non-null) receives
// the project-relative paths actually read (a relDocs entry naming no openable
// file is silently excluded — INV-15).
QList<Finding> check(const QString &rootCanonical,
                     const QStringList &relDocs,
                     const Options &opts = {},
                     QStringList *checkedDocs = nullptr);

// GitHub-compatible heading anchor slug (`github-slugger` algorithm): strip
// backtick code-span markers, lowercase, delete every char that is not a
// letter/digit/space/hyphen/underscore (punctuation AND emoji/symbols go),
// then spaces→'-' without collapsing runs or trimming. `seen` carries the
// duplicate-count state across a doc's headings (2nd identical base → `-1`).
QString gfmSlug(const QString &headingText, QHash<QString, int> &seen);
// Convenience: a standalone slug with fresh duplicate state.
QString gfmSlug(const QString &headingText);

}  // namespace DocIntegrity
