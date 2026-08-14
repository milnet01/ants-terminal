// ANTS-3601 — deterministic doc-integrity engine. Qt6::Core only, FS-reading,
// no widgets, no subprocess (mirrors docsindex.cpp / codebaseindex.cpp). Finds
// these classes of internal-consistency rot in markdown contract docs — all
// computable with no LLM (the count is deliberately not written out: it has
// been stale twice, at three while the list held four):
//
//   1. Dead anchors  — a `[t](#slug)` / `[t](other.md#slug)` whose slug names
//                      no real heading.
//   2. Broken links  — a `[t](relpath)` whose target file does not exist.
//   3. TOC coverage  — a hand-maintained Table of Contents that omits a
//                      section, or lists a duplicate/dead entry.
//   4. Heading order — numbered headings that run out of order, skip a
//                      number, or reuse one (ANTS-3700).
//   5. Ungranted tool — a Claude Code skill whose body calls an MCP verb its
//                      own `allowed-tools:` frontmatter never granted, so the
//                      skill is unexecutable as written (ANTS-3719).
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
    // ANTS-3700 — a numbered heading (`## 5.7 Foo`) that is lower than the
    // sibling before it, skips a number no sibling ever fills, or repeats one.
    // Every anchor still resolves and the reader's eye reconstructs the
    // intended order, so this class survives review: spec-format.md — the
    // standard that DEFINES section ordering — shipped with 5.8 before 5.7 and
    // cleared three cold-eyes loops, two at full model.
    HeadingSequence,
    // ANTS-3719 — a skill's `allowed-tools:` frontmatter omits an MCP verb its
    // own body tells you to call. Requested by the claude_config session, which
    // paid for this twice at cold-reader prices in one skill: loop 2 of a
    // review found `doc_integrity` ungranted, loop 4 found `doc_citations`
    // ungranted again, reintroduced by a commit that added the dependency and
    // never touched the frontmatter. Both times the deterministic pre-pass that
    // skill mandates was unexecutable as declared — the gate that catches
    // everything else, itself broken.
    UngrantedTool,
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
