// ANTS-1305 — similar_code pattern matcher.
//
// Walks a project's source tree, extracts class/function signatures by
// reusing the FileOutline extractor, and ranks them by token-set
// Jaccard similarity to a free-text `shape` query. Answers "show me
// the project's existing examples of this shape" before Claude writes a
// new dialog / IPC verb / test — the CLAUDE.md §3 reuse-before-rewriting
// rule, surfaced as a tool.
//
// Sibling to symbolquery.{h,cpp} (whose tree-walk shape this mirrors)
// and fileoutline.{h,cpp} (whose extractor this reuses). Qt6::Core only,
// no GUI deps — lives in ants_core_lib so it reaches the MCP/IPC
// dispatch path. The MCP/IPC caller resolves + anchors the project
// root; the lib never escapes that subtree (it does not follow
// symlinks). The `shape` query is tokenised, never spliced into a
// regex, so there is no injection surface and no identifier guard.
//
// See docs/specs/ANTS-1305.md.

#ifndef ANTS_SIMILARCODE_H
#define ANTS_SIMILARCODE_H

#include <QString>
#include <QStringList>
#include <QVector>

namespace SimilarCode {

// v1 covers the families FileOutline extracts code symbols for.
enum class Lang { Auto, Cpp, Py };

// "" / "auto" → Auto; "cpp"/"py" → the explicit family; anything else
// → Auto (permissive — the caller's lang hint never hard-fails).
Lang parseLang(const QString &s);

struct Match {
    QString file;       // project-relative (root prefix stripped)
    int     line = 0;
    QString signature;  // the matched signature line (trimmed by FileOutline)
    QString kind;       // FileOutline kind: "class" / "func" / "qt"
    QString lang;       // "cpp" / "py"
    double  score = 0.0;// Jaccard similarity 0..1 (rounded to 4 dp)
};

struct Options {
    // 0 is the sentinel "use the default" (NOT "return nothing"):
    // maxResults → 3 (capped at 20), maxFiles → 5000.
    int  maxResults = 0;
    int  maxFiles   = 0;
    Lang lang       = Lang::Auto;
};

struct Result {
    bool ok = true;
    QString error;
    QString code;
    QVector<Match> matches;
    int  matchesTotal = 0;   // pre-cap count of score>0 candidates
    int  filesScanned = 0;
    bool truncated  = false; // cap dropped entries
    bool walkCapped = false; // maxFiles cap hit
};

// Lowercase, split on runs of non-[A-Za-z0-9], drop tokens < 2 chars.
QStringList tokenize(const QString &shape);

// |set(a) ∩ set(b)| / |set(a) ∪ set(b)|; 0 when the union is empty.
double jaccard(const QStringList &a, const QStringList &b);

// Takes an already-canonical project root. Refuses with code
// "bad_args" on an empty / over-long / token-empty shape (defensive —
// the MCP/IPC handler validates first). Empty/non-dir root yields an
// empty ok:true result (the handler is expected to gate "no_project").
Result findSimilar(const QString &rootCanonical,
                   const QString &shape,
                   const Options &opts);

}  // namespace SimilarCode

#endif  // ANTS_SIMILARCODE_H
