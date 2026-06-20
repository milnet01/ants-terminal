// ANTS-1303 — find_definition / find_caller regex scanner.
//
// Bounded recursive regex scan over a project's source tree, answering
// the two most-frequent code-navigation questions — "where is `Foo`
// defined?" and "what calls `Foo`?" — without a clangd / tree-sitter /
// build-system dependency. C++ / Python / Lua / Shell aware.
//
// Sibling to fileoutline.{h,cpp} (single-file outline); this one walks
// the tree. Qt6::Core only, no GUI deps — lives in ants_core_lib so it
// reaches the MCP/IPC dispatch path. The MCP/IPC caller resolves +
// anchors the project root; the lib never escapes that subtree (it
// does not follow symlinks).
//
// See docs/specs/ANTS-1303.md.

#ifndef ANTS_SYMBOLQUERY_H
#define ANTS_SYMBOLQUERY_H

#include <QString>
#include <QVector>

#include <optional>

namespace SymbolQuery {

// ANTS-2150 — `Generic` is the shared brace-family family (Rust, Go, JS/TS,
// Java, C#, Kotlin, Swift, Scala, PHP); its anchors mirror FileOutline's
// generic outline regexes so find_definition / find_caller cover the same
// files codebase_index / file_outline now do.
enum class Lang { Auto, Cpp, Py, Lua, Sh, Generic };

// "" / "auto" → Auto; "cpp"/"py"/"lua"/"sh"/"generic" → the explicit family;
// anything else → Auto (permissive — the caller's lang hint never
// hard-fails the scan).
Lang parseLang(const QString &s);

// Identifier guard: ^[A-Za-z_][A-Za-z0-9_]{0,127}$ (ASCII only).
// Rejects empty, leading digit, regex metachars, and >128 chars. The
// symbol is the only caller-supplied text spliced into a regex, so
// this is the security boundary (QRegularExpression::escape is applied
// on top as defence in depth).
bool isValidSymbol(const QString &symbol);

struct DefMatch {
    QString file;       // project-relative (root prefix stripped)
    int     line = 0;
    QString signature;  // trimmed matched line
    QString lang;       // "cpp" / "py" / "lua" / "sh"
    QString kind;       // "definition" | "declaration"
};

struct CallMatch {
    QString file;       // project-relative
    int     line = 0;
    QString context;    // trimmed matched line
    QString lang;
};

struct Options {
    // 0 is the sentinel "use the tool-specific default" (NOT "cap at
    // zero / return nothing"): maxResults → 50 (def) / 200 (call),
    // maxFiles → 5000.
    int  maxResults = 0;
    int  maxFiles   = 0;
    Lang lang       = Lang::Auto;
};

struct DefResult {
    bool ok = true;
    QString error;
    QString code;
    QVector<DefMatch> definitions;
    int  definitionsTotal = 0;   // pre-cap count of all matches
    int  filesScanned     = 0;
    bool truncated  = false;     // cap dropped entries
    bool walkCapped = false;     // maxFiles cap hit
    // ANTS-1950 — when no definition matched but the query equals a source
    // file's base name, the project-relative path of that file ("" = none).
    // Lets the handler answer "no symbol X; did you mean the file X.cpp?".
    QString fileStemHint;
};

struct CallResult {
    bool ok = true;
    QString error;
    QString code;
    QVector<CallMatch> callers;
    std::optional<DefMatch> definition;  // best def, for the "where + who" case
    int  callersTotal = 0;
    int  filesScanned = 0;
    bool truncated  = false;
    bool walkCapped = false;
};

// Both take an already-canonical project root; both refuse with
// code "bad_args" on an invalid symbol (defensive — the MCP/IPC
// handler validates first). Empty/non-dir root yields an empty
// ok:true result (the handler is expected to gate "no_project").
DefResult  findDefinition(const QString &rootCanonical,
                          const QString &symbol,
                          const Options &opts);
CallResult findCaller(const QString &rootCanonical,
                      const QString &symbol,
                      const Options &opts);

}  // namespace SymbolQuery

#endif  // ANTS_SYMBOLQUERY_H
