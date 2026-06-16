// ANTS-2139 — docs_index MCP tool: a pre-computed, project-agnostic
// documentation map. Pure helper (Qt6::Core-only, FS-reading, no widgets /
// no subprocess — mirrors codebaseindex.h / findsources.h). Walks
// <root>/*.md (non-recursive) + <root>/docs/**/*.md (recursive), scans each
// for its heading outline / first-H1 title / best-effort **Status:** /
// outbound relative-.md links, caches the result to
// ~/.cache/ants-terminal/docs-index/<hash>.json, and serves
// summary / topic= / doc_path= / id= queries so a session stops re-deriving
// the doc shape with grep / Read across an unfamiliar project layout.
//
// The SOURCE-map sibling is codebase_index (ANTS-1637); this is the DOC-map
// over all doc types and project layouts (rich docs/ trees, flat top-level,
// or none). The thin cmdDocsIndex wrapper (remotecontrol.cpp) resolves the
// project root + the caller_cwd contract + PathValidation, then drives
// serve() here. See docs/specs/ANTS-2139.md.

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DocsIndex {

constexpr int    kIndexVersion      = 1;
constexpr int    kMaxIndexDocs      = 2000;            // INV-12 build doc-count ceiling
constexpr qint64 kMaxCacheBytes     = 8 * 1024 * 1024; // INV-15 hard cache/heap ceiling (8 MiB)
constexpr int    kMaxHeadingsPerDoc = 1000;            // INV-19 silent per-doc cap
constexpr int    kMaxLinksPerDoc    = 500;             // INV-19 silent per-doc cap
constexpr int    kMaxTopicHits      = 50;              // INV-6 topic= response cap
constexpr int    kMaxLinkedFrom     = 200;             // INV-18 doc_path= reverse-edge cap
constexpr int    kMaxLineBytes      = 1024;            // INV-3 fixed regex-skip guard (not Options-overridable; mirrors FileOutline)
constexpr qint64 kMaxDocBytes       = 4 * 1024 * 1024; // INV-19 per-doc read budget (ROADMAP/CHANGELOG can be MB-scale)

struct Heading {
    int     level = 0;   // 1..6 = ATX '#' count
    QString text;        // heading text, trimmed (trailing run of '#' stripped)
    int     line = 0;    // 1-based
};

struct DocEntry {
    QString          path;       // project-relative ("docs/specs/ANTS-1637.md")
    QString          id;         // filename stem ("ANTS-1637"); never empty
    QString          title;      // first H1 text; "" when the doc has no H1
    QString          status;     // best-effort **Status:** value; "" when absent (INV-17)
    int              lines = 0;
    qint64           mtimeMs = 0;
    QVector<Heading> headings;   // capped at maxHeadingsPerDoc (silent, INV-19)
    QStringList      links;      // outbound resolved project-relative .md targets, deduped + sorted
};

struct Index {
    int                version = kIndexVersion;
    QString            rootCanonical;
    qint64             generatedAtMs = 0;        // injected, not clock-read (testability)
    QVector<DocEntry>  docs;                      // sorted by path: root *.md then docs/
    bool               docsTruncated = false;     // INV-12/INV-15 build ceiling hit
};

struct Options {                       // all overridable for tests
    int    maxIndexDocs      = kMaxIndexDocs;
    qint64 maxCacheBytes     = kMaxCacheBytes;
    int    maxHeadingsPerDoc = kMaxHeadingsPerDoc;
    int    maxLinksPerDoc    = kMaxLinksPerDoc;
    int    maxTopicHits      = kMaxTopicHits;
    int    maxLinkedFrom     = kMaxLinkedFrom;
    qint64 maxDocBytes       = kMaxDocBytes;
};

// One query's selectors (§ 2.5): at most one member non-empty. The handler
// rejects ≥2 non-empty with bad_args *before* calling query().
struct QueryParams {
    QString topic;
    QString docPath;   // project-relative (validated by the handler)
    QString id;
};

// What changed on disk vs `prev`: docs whose mtime differs (changed), docs on
// disk absent from `prev` (added), `prev` docs gone from disk (removed).
// Staleness is mtime-only — a doc set has no subsystems.md analogue.
struct StaleSet {
    QStringList changed, added, removed;
    bool        any() const {
        return !changed.isEmpty() || !added.isEmpty() || !removed.isEmpty();
    }
};

// Cold build: walk <root>/*.md (non-recursive) then <root>/docs/**/*.md
// (recursive), scan each, sort by path. Stops at maxIndexDocs OR when the
// serialised index would exceed maxCacheBytes (whichever first), setting
// docsTruncated. generatedAtMs passed in (no internal clock read).
Index build(const QString &rootCanonical, qint64 generatedAtMs,
            const Options &opts = {});

StaleSet staleDocs(const Index &prev, const QString &rootCanonical);

// Re-scan only changed+added, drop removed, reuse prev's entries for untouched.
// *refreshedOut = changed+added count.
Index refresh(const Index &prev, const QString &rootCanonical,
              qint64 generatedAtMs, const Options &opts, int *refreshedOut);

// Build the response body (without the dispatcher-injected etag) for one
// query. refreshedDocs + cachePath ride the meta block. ≥2 selectors →
// bare {ok:false, code:bad_args}.
QJsonObject query(const Index &idx, const QueryParams &params,
                  int refreshedDocs, const QString &cachePath,
                  const Options &opts = {});

QJsonObject toJson(const Index &idx);
Index       fromJson(const QJsonObject &obj);

// ~/.cache/ants-terminal/docs-index/<cwdHash(root)>.json.
QString cachePathFor(const QString &rootCanonical);

// Orchestrate load → refresh → write-back → query. Cold-builds when the cache
// is absent / unparseable / version- or root-mismatched. nowMs injected for
// testability; cachePathOverride empty → cachePathFor(root). Forwards
// refresh's out-count + cachePathFor(root) into query().
QJsonObject serve(const QString &rootCanonical, qint64 nowMs,
                  const QueryParams &params, const Options &opts = {},
                  const QString &cachePathOverride = {});

}  // namespace DocsIndex
