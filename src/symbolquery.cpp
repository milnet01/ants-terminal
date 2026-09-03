// ANTS-1303 — find_definition / find_caller regex scanner.
// See symbolquery.h header.

#include "symbolquery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVarLengthArray>

namespace SymbolQuery {

namespace {

constexpr int kMaxLineBytes       = 1024;   // per-line regex-skip cap
constexpr int kDefaultMaxFiles    = 5000;
constexpr int kDefDefaultResults  = 50;
constexpr int kCallDefaultResults = 200;
constexpr int kSymbolMaxLen       = 128;

bool isAsciiAlpha(QChar c) {
    const ushort u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}
bool isAsciiAlnum(QChar c) {
    const ushort u = c.unicode();
    return isAsciiAlpha(c) || (u >= '0' && u <= '9');
}

const char *langStr(Lang l) {
    switch (l) {
        case Lang::Cpp:     return "cpp";
        case Lang::Py:      return "py";
        case Lang::Lua:     return "lua";
        case Lang::Sh:      return "sh";
        case Lang::Generic: return "generic";
        case Lang::Glsl:    return "glsl";
        case Lang::Auto:    return "";
    }
    return "";
}

// Map a lowercased extension to its language family; Auto == "skip".
Lang langForExt(const QString &ext) {
    if (ext == QLatin1String("cpp") || ext == QLatin1String("cc")  ||
        ext == QLatin1String("cxx") || ext == QLatin1String("c")   ||
        ext == QLatin1String("h")   || ext == QLatin1String("hpp") ||
        ext == QLatin1String("hh")  || ext == QLatin1String("hxx")) {
        return Lang::Cpp;
    }
    if (ext == QLatin1String("py") || ext == QLatin1String("pyi")) return Lang::Py;
    if (ext == QLatin1String("lua")) return Lang::Lua;
    if (ext == QLatin1String("sh") || ext == QLatin1String("bash")) return Lang::Sh;
    // ANTS-2150 — brace family + Ruby (kept in step with
    // FileOutline::genericLangName + CodebaseIndex::isIndexableSuffix so
    // count → outline → symbol query agree). Ruby (`.rb`) reuses Lang::Generic:
    // `def`/`class`/`module` are in the keyword alternation; the Generic def
    // anchor carries an optional `self.` receiver for singleton methods.
    if (ext == QLatin1String("rs")  || ext == QLatin1String("go")  ||
        ext == QLatin1String("js")  || ext == QLatin1String("jsx") ||
        ext == QLatin1String("mjs") || ext == QLatin1String("cjs") ||
        ext == QLatin1String("ts")  || ext == QLatin1String("tsx") ||
        ext == QLatin1String("java")|| ext == QLatin1String("cs")  ||
        ext == QLatin1String("kt")  || ext == QLatin1String("kts") ||
        ext == QLatin1String("swift")|| ext == QLatin1String("scala") ||
        ext == QLatin1String("sc")  || ext == QLatin1String("php") ||
        ext == QLatin1String("rb")) {
        return Lang::Generic;
    }
    // ANTS-3558 — GLSL / Vulkan shader stages. Only the UNAMBIGUOUS
    // extensions: the two-letter `.vs`/`.fs`/`.gs` are deliberately omitted
    // because `.fs` is also F# source — `.vert`/`.frag`/`.geom` cover those
    // stages without the cross-language collision.
    if (ext == QLatin1String("glsl") || ext == QLatin1String("comp") ||
        ext == QLatin1String("frag") || ext == QLatin1String("vert") ||
        ext == QLatin1String("geom") || ext == QLatin1String("tesc") ||
        ext == QLatin1String("tese") || ext == QLatin1String("vsh")  ||
        ext == QLatin1String("fsh")  || ext == QLatin1String("mesh") ||
        ext == QLatin1String("task") || ext == QLatin1String("rgen") ||
        ext == QLatin1String("rchit")|| ext == QLatin1String("rmiss")||
        ext == QLatin1String("rahit")|| ext == QLatin1String("rint") ||
        ext == QLatin1String("rcall")) {
        return Lang::Glsl;
    }
    return Lang::Auto;
}

// ANTS-4369 — the declaration test is "does the line end in `;`", and a
// trailing comment defeated it: two adjacent prototypes in one header were
// classified oppositely on nothing but a `// note`. Cut from the first `//`
// that is NOT inside a string or char literal, drop a trailing `/* … */`,
// then test. It compounds ANTS-4368 when both are live — that bug drops the
// real definition and this one relabels the surviving prototype AS the
// definition, so the envelope holds one result marked `definition` pointing
// at a line with no body, and nothing suggests the answer is incomplete.
//
// Returns the line with comments removed, so both tests below read code
// rather than prose.
QString codeWithoutComments(const QString &line) {
    QString code;
    code.reserve(line.size());
    QChar quote;                       // null when outside a literal
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) {          // skip the escaped char
                code.append(c);
                if (i + 1 < line.size()) code.append(line.at(++i));
                continue;
            }
            if (c == quote) quote = QChar();
            code.append(c);
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
            code.append(c);
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < line.size()) {
            const QChar n = line.at(i + 1);
            if (n == QLatin1Char('/')) break;      // line comment: done
            if (n == QLatin1Char('*')) {
                const int close = line.indexOf(QStringLiteral("*/"), i + 2);
                if (close < 0) break;              // unterminated: rest is comment
                i = close + 1;
                continue;
            }
        }
        code.append(c);
    }
    return code.trimmed();
}

// True when the `[…]` ending `head` is a lambda's capture list rather than an
// array extent. `int m_sizes[3]` opens its bracket against the declarator;
// `auto f = []` cannot.
bool endsWithCaptureList(const QString &head) {
    if (!head.endsWith(QLatin1Char(']'))) return false;
    int depth = 0;
    for (int i = head.size() - 1; i >= 0; --i) {
        const QChar c = head.at(i);
        if (c == QLatin1Char(']')) ++depth;
        else if (c == QLatin1Char('[') && --depth == 0)
            return i == 0 || !(head.at(i - 1).isLetterOrNumber()
                               || head.at(i - 1) == QLatin1Char('_'));
    }
    return false;
}

// A line is a DECLARATION when it ends in `;` and opens no body. ANTS-4358
// found the first half of the brace edge: `static const auto f = [](int n) {
// … };` ends in `;` and is a definition, and so is a one-line `struct Foo {
// int a; };`. ANTS-4821 found the other half — a brace also INITIALISES:
//
//     QTimer *m_timer{nullptr};      declaration
//     int m_scrollOffset = 0;        declaration — the same member, and the
//                                    two spellings used to disagree
//
// What precedes the first brace separates them. A body follows a parameter
// list, a capture list or a class-key; an initialiser follows the declarator
// it abuts.
bool looksLikeDeclaration(const QString &line) {
    const QString code = codeWithoutComments(line);
    if (!code.endsWith(QLatin1Char(';'))) return false;
    const qsizetype brace = code.indexOf(QLatin1Char('{'));
    if (brace < 0) return true;

    const QString head = code.left(brace).trimmed();
    // A parameter list anywhere before the brace: `int f() const { … }`,
    // `auto f = [](int n) { … }`. A `)` cannot precede an initialiser's brace,
    // because that brace is the first one on the line.
    if (head.contains(QLatin1Char(')'))) return false;
    if (endsWithCaptureList(head)) return false;
    static const QStringList kClassKeys{
        QStringLiteral("struct"),  QStringLiteral("class"),
        QStringLiteral("union"),   QStringLiteral("enum"),
        QStringLiteral("typedef"), QStringLiteral("template")};
    return !kClassKeys.contains(head.left(head.indexOf(QLatin1Char(' '))));
}

// Per-call compiled anchors for one language. `def` holds 1-2 patterns
// (a line is a definition if any matches); `call` is the call anchor.
// `cppKind` selects the trailing-`;` declaration/definition split.
struct Anchors {
    QVector<QRegularExpression> def;
    QRegularExpression call;
    // ANTS-4603 — the two halves of the wrapped-signature anchor. Every
    // pattern in `def` matches ONE line, and a C++ member definition whose
    // return type is long conventionally puts that type on its own line:
    //
    //     std::optional<RemoteControl::RoadmapWriteTarget>
    //     RemoteControl::roadmapWriteTarget(const QString &projectRoot,
    //
    // The second line carries no return-type token, so no `def` pattern can
    // match it, and the symbol resolved to nothing at all — which reads as
    // "no such symbol" rather than "not found", so a caller designs around a
    // function that is there. `wrappedDef` matches the qualified-name line;
    // `wrappedPrev` is what the line ABOVE must look like for it to count.
    // Empty for every language but C++.
    QRegularExpression wrappedDef;
    QRegularExpression wrappedPrev;
    bool hasWrapped = false;
    bool cppKind = false;
    const char *lang = "";
};

Anchors buildAnchors(Lang lang, const QString &s) {
    Anchors a;
    a.lang = langStr(lang);
    auto add = [&](const QString &pat) {
        QRegularExpression re(pat);
        re.optimize();
        a.def.append(re);
    };
    switch (lang) {
        case Lang::Cpp:
            // ANTS-1700 — a definition/declaration requires a return-type
            // token before the (optionally qualified) name, so a
            // namespace-qualified *call* site (`ns::sym(`) is no longer
            // mis-reported as a definition. Each `[\w:<>~]+` token is
            // followed by a real separator `[\s*&]+` (whitespace / `*` /
            // `&`); one-or-more such tokens form the return type +
            // specifiers, then an optional `(?:[\w:]+::)` class qualifier,
            // then the name. A bare call (`sym(`) and a qualified call
            // (`ns::sym(`) both lack the leading return-type token, so
            // neither matches. The per-line byte cap (kMaxLineBytes)
            // bounds backtracking.
            // ANTS-2146 — a statement-position call (`return sym(`,
            // `throw sym(`, `else sym(` …) DOES have a leading word token
            // (the keyword) + a space, which the return-type group would
            // otherwise absorb, mis-tagging the call as a `declaration`.
            // Reject when the first token is an expression-introducing
            // reserved keyword: a keyword is never a return type, so the
            // negative lookahead has zero false negatives.
            // ANTS-4368 — an optional linkage prefix ahead of the return
            // type. `extern "C" int foo(void)` failed the return-type group
            // because `"` is not in `[\w:<>~]`, so a C/C++ project's whole
            // cross-language seam resolved only to its header prototype —
            // and reported ok:true with definitions_count:1, which is a
            // confident wrong answer rather than an empty one. The slot also
            // takes `static`/`inline`/`__declspec(...)` later without another
            // change. Measured by DOOM: 48 such definitions in one file.
            // ANTS-3746 — an optional template argument list per return-type
            // token. A COMMA is neither in `[\w:<>~]` nor a separator, so the
            // group could not span `QMap<QString, QStringList>` and the
            // definition was reported absent — indistinguishable from "no such
            // symbol", so the caller designs around a function that is there.
            // Measured on this tree: `detectorsByCategory`, declared at
            // debtsweepengine.h:264 and defined at debtsweepengine.cpp:1344,
            // returned definitions_count:0 over 911 files.
            // `[^();{]*` is deliberately blind to `<>` balance rather than
            // counting it: the trailing `SYMBOL\s*\(` anchors where the list
            // ends, so arbitrary nesting (`map<K, vector<pair<A, B>>>`) works
            // with no recursion. Excluding `(`/`)`/`;`/`{` is what stops it
            // reaching across a statement boundary into a later template.
            // The base class keeps `<>`, so every pre-ANTS-3746 line still
            // matches by the same path it did; the new group is only reached
            // by backtracking, when the greedy token stops at a comma.
            add(QStringLiteral("^[ \\t]*(?:extern\\s*\"C(?:\\+\\+)?\"\\s+)?(?!(?:return|co_return|co_await|co_yield|throw|else)\\b)(?:[\\w:<>~]+(?:<[^();{]*>)?[\\s*&]+)+(?:[\\w:]+::)?") + s + QStringLiteral("\\s*\\("));
            // Out-of-line constructor / destructor definitions carry no
            // return type (`Foo::Foo(` / `Foo::~Foo(`); match them
            // explicitly so a class query still resolves its ctor/dtor.
            // (In-class ctor *declarations* — `Foo(` with no qualifier —
            // are indistinguishable from a call and intentionally left out.)
            add(QStringLiteral("^[ \\t]*") + s + QStringLiteral("::~?") + s + QStringLiteral("\\s*\\("));
            // ANTS-3465 — a type definition (`struct|class|union|enum[ class]
            // Foo`) carries no return type and no `(`, so neither pattern
            // above matched it: a pure type (no same-named ctor) returned
            // definitions_count:0. Match the keyword-led form, allowing an
            // optional `template<…>` / `typedef` prefix and one optional
            // ALL-CAPS export-macro token (`class QT_EXPORT Foo`). scanFile's
            // existing kind logic tags `Foo {` → definition, `Foo;` (forward
            // decl) → declaration, with no schema change.
            add(QStringLiteral("^[ \\t]*(?:template\\s*<[^>]*>\\s*)?(?:typedef\\s+)?(?:struct|class|union|enum(?:\\s+class|\\s+struct)?)\\s+(?:[A-Z_][A-Z0-9_]*\\s+)?") + s + QStringLiteral("\\b"));
            // ANTS-4358 — a lambda assigned to `auto` inside a function body.
            // ANTS-4090 added the brace family's top-level `const NAME =`;
            // C++ spells the same idea with `auto` and nests it, which is how
            // this codebase defines its shared MCP schema helpers
            // (makeEtagMatchProp, makeFieldsProp, makeRawProp, makeDryRunProp)
            // — four symbols `mcp-tools.md` cites by name and the resolver
            // could not find. NOT anchored to column 0: the whole point is
            // that these live at function-body indent.
            add(QStringLiteral("^\\s*(?:static\\s+)?(?:const\\s+)?(?:constexpr\\s+)?auto\\s+") + s + QStringLiteral("\\s*=\\s*\\["));
            // ANTS-3668 — a DATA MEMBER declaration. The three forms above
            // all require either a `(` (function) or a type keyword (class),
            // so every struct field, every `m_`-prefixed member, resolved
            // nowhere. Measured via doc_symbols' corpus calibration as the
            // single largest classified population of unresolved candidates
            // — the one that fires ANTS-3661 § 2.1's "something is missing"
            // gate.
            //
            // Shape: one or more type tokens, then the name, then a
            // terminator of `;`, `=` or `{`. The absence of `(` before that
            // terminator is what separates this from a function declaration
            // and from a call, and it is load-bearing rather than incidental.
            // An optional array extent (`int m_rows[8];`) sits between.
            //
            // The template-argument group is the same one ANTS-3746 added to
            // the return-type ladder, and it is needed here for the same
            // reason: a COMMA is neither a type character nor a separator, so
            // without it `QMap<QString, QStringList> m_byCategory;` does not
            // match.
            //
            // This DOES also match a local variable, and that is a decision
            // rather than an oversight. The ladder is line-based with no
            // scope tracking, so "inside a class body" is not a question it
            // can ask; a local is a real declaration of that name, and the
            // existing kind logic tags both `declaration` because both end in
            // `;`. Reporting where a name is declared beats reporting that a
            // name the reader is looking at does not exist.
            //
            // The keyword lookahead is the same guard the return-type form
            // carries, widened by the statement keywords that can precede a
            // bare name (`case Foo:` and `using Foo =` are not declarations
            // of Foo in the sense a reader means).
            add(QStringLiteral(
                    "^[ \\t]*(?!(?:return|co_return|co_await|co_yield|throw|else|case|"
                    "default|using|typedef|friend|delete|goto|sizeof|new)\\b)"
                    "(?:[\\w:<>~]+(?:<[^();{]*>)?[\\s*&]+)+(?:[\\w:]+::)?")
                + s
                + QStringLiteral("\\s*(?:\\[[^\\]]*\\])?\\s*[;={]"));
                        // ANTS-4346 — a namespace. `find_definition` returned an empty
            // `definitions[]` for one, which reads as "no such symbol" for a
            // thing that is plainly there. Tagged `namespace` by the kind
            // logic below so a caller can tell it from a function.
            add(QStringLiteral("^[ \\t]*namespace\\s+") + s + QStringLiteral("\\b"));
            // ANTS-4603 — the wrapped-return-type pair (see Anchors). Both
            // halves are deliberately narrow, because the qualified-name line
            // on its own is indistinguishable from a statement-position call
            // (`Foo::bar(x);`), which is what kept these patterns single-line.
            //
            // Anchored at COLUMN 0, with no `[ \t]*` slack: an out-of-line
            // definition sits at column 0 and a call inside a function body is
            // always indented. Measured over this tree — 30 wrapped
            // definitions at column 0, 0 indented — so the strict anchor costs
            // nothing here and removes the whole indented-call population.
            a.wrappedDef = QRegularExpression(
                QStringLiteral("^(?:[A-Za-z_]\\w*::)+") + s + QStringLiteral("\\s*\\("));
            // And the line above must read as a return type: one token run of
            // type characters, ending in `>`, a word character, `*` or `&`.
            // The lookahead rejects a keyword, which is never a return type,
            // so `return` / `throw` / `else` on their own line cannot license
            // the call below them. A line ending `&&` is excluded in code —
            // it is a wrapped boolean expression, not a reference return.
            a.wrappedPrev = QRegularExpression(QStringLiteral(
                "^(?!(?:return|co_return|co_await|co_yield|throw|else|case|default)\\b)"
                "[A-Za-z_][\\w:<>,~ \\t*&]*[>\\w*&]$"));
            a.wrappedDef.optimize();
            a.wrappedPrev.optimize();
            a.hasWrapped = true;
            a.call = QRegularExpression(QStringLiteral("\\b") + s + QStringLiteral("\\s*\\("));
            a.cppKind = true;
            break;
        case Lang::Py:
            add(QStringLiteral("^\\s*(?:async\\s+)?(?:def|class)\\s+") + s + QStringLiteral("\\b"));
            a.call = QRegularExpression(QStringLiteral("\\b") + s + QStringLiteral("\\s*\\("));
            break;
        case Lang::Lua:
            add(QStringLiteral("^\\s*(?:local\\s+)?function\\s+(?:[\\w.:]*[.:])?") + s + QStringLiteral("\\s*\\("));
            add(QStringLiteral("^\\s*(?:local\\s+)?") + s + QStringLiteral("\\s*=\\s*function\\b"));
            a.call = QRegularExpression(QStringLiteral("\\b") + s + QStringLiteral("\\s*\\("));
            break;
        case Lang::Sh:
            add(QStringLiteral("^\\s*(?:function\\s+)?") + s + QStringLiteral("\\s*\\(\\s*\\)"));
            add(QStringLiteral("^\\s*function\\s+") + s + QStringLiteral("\\b"));
            a.call = QRegularExpression(QStringLiteral("\\b") + s + QStringLiteral("\\b"));
            break;
        case Lang::Generic:
            // ANTS-2150 — mirror FileOutline's three brace-family patterns,
            // spliced around the (escaped) query symbol `s`:
            // (1) keyword declaration (`pub fn s`, `class s`, `func (r R) s`,
            //     `def self.s` Ruby singleton — ANTS-2150 optional receiver),
            add(QStringLiteral("^\\s*(?:(?:pub|export|default|public|private|protected|internal|static|final|abstract|sealed|async|open|override|suspend|inline|const|unsafe|extern|data)\\s+)*(?:fn|fun|func|function|def|class|struct|enum|trait|impl|interface|type|module|object|protocol|extension|namespace|record)\\b(?:\\s+\\([^)]*\\))?\\s+(?:self\\.)?") + s + QStringLiteral("\\b"));
            // (2) C-style method definition (`private void s(...) {`),
            add(QStringLiteral("^\\s*(?!(?:return|if|for|while|switch|catch|else|throw|new|await|do|in|of)\\b)(?:[A-Za-z_$<>\\[\\].]+[\\s*&]+)+") + s + QStringLiteral("\\s*\\([^;{]*\\)\\s*(?:->\\s*[\\w$<>\\[\\].?]+\\s*|:\\s*[\\w$<>\\[\\].?]+\\s*)?\\{"));
            // (3) JS/TS arrow-function assignment (`const s = (...) =>`).
            add(QStringLiteral("^\\s*(?:export\\s+)?(?:default\\s+)?(?:const|let|var)\\s+") + s + QStringLiteral("\\s*=\\s*(?:async\\s+)?(?:\\([^)]*\\)|[A-Za-z_$][\\w$]*)\\s*=>"));
            a.call = QRegularExpression(QStringLiteral("\\b") + s + QStringLiteral("\\s*\\("));
            break;
        case Lang::Glsl:
            // ANTS-3558 — a GLSL function definition/prototype is
            // `<qualifiers>* <rettype> <name>(<args>)` at line start. Require
            // one-or-more leading `word ` tokens (the return type, plus any
            // storage/precision qualifiers) BEFORE the name, then `(`. This
            // stops at `(` — it does NOT require `{` on the same line, so the
            // Allman next-line-brace style (`uint pcgHash(uint x)\n{`) common
            // in shader code still resolves (the Generic brace anchor misses
            // it). The negative lookahead rejects a statement-position call
            // (`return foo(`, `else foo(`): a bare/qualified call has no
            // leading return-type token, so it never matches. cppKind splits
            // a trailing-`;` prototype into `declaration`.
            add(QStringLiteral("^[ \\t]*(?!(?:return|else|if|for|while|do|switch|case|discard)\\b)(?:[A-Za-z_]\\w*\\s+)+") + s + QStringLiteral("\\s*\\("));
            a.call = QRegularExpression(QStringLiteral("\\b") + s + QStringLiteral("\\s*\\("));
            a.cppKind = true;
            break;
        case Lang::Auto:
            break;
    }
    a.call.optimize();
    return a;
}

// Mutable accumulators threaded through the recursive walk.
struct ScanState {
    int rootPrefixLen = 0;
    Lang langFilter = Lang::Auto;
    int maxFiles = kDefaultMaxFiles;
    int filesScanned = 0;
    bool walkCapped = false;

    // Anchors keyed by language ordinal; built once per call.
    // Size == Lang enum cardinality
    // (Auto,Cpp,Py,Lua,Sh,Generic — ANTS-2150; Glsl — ANTS-3558).
    Anchors anchors[7];

    // Definition buckets (definitions first, then declarations).
    bool collectDefs = false;
    int  defCap = 0;
    QVector<DefMatch> defsDefn;
    QVector<DefMatch> defsDecl;
    int  defsTotal = 0;

    // Caller bucket.
    bool collectCalls = false;
    int  callCap = 0;
    QVector<CallMatch> calls;
    int  callsTotal = 0;

    // ANTS-1950 — file-stem fallback hint. When a query matches no symbol
    // but exactly equals a source file's base name (e.g. asking for
    // `test_reference_harness`, which is a filename not a symbol), record
    // the first such file so the caller gets a "did you mean the file?"
    // nudge instead of a bare zero result.
    QString symbol;       // the queried symbol, for stem comparison
    QString fileStemHit;  // first rel path whose base name == symbol ("" = none)

    // ANTS-3680 — batch mode. When `batch` is non-null this state owns the
    // WALK only (file count, cap, stem hints) and carries no symbol of its
    // own: scanFile fans each line out to the needles that line holds.
    // holds. `bySymbol` keys on views into the caller's stable needle list.
    QVector<ScanState> *batch = nullptr;
    const QHash<QStringView, int> *bySymbol = nullptr;
    const QHash<QString, int> *byStem = nullptr;   // lowercased needle -> slot
};

// ANTS-3680 — the batch gate. Every anchor bounds the symbol on both sides
// (a separator, `::`, `~` or line start before it; `\b`, `(`, `=` or one of
// `;={` after), so a needle embedded in a longer identifier matches nothing.
// Whole-identifier tokens are therefore exactly the `contains` test the
// single-symbol path runs, at a cost independent of the needle count.
void needleSlots(const QHash<QStringView, int> &bySymbol, const QString &line,
                 QVarLengthArray<int, 8> &out) {
    out.clear();
    const QStringView v(line);
    qsizetype i = 0;
    while (i < v.size()) {
        const QChar c = v.at(i);
        if (!isAsciiAlpha(c) && c != QLatin1Char('_')) { ++i; continue; }
        qsizetype j = i + 1;
        while (j < v.size() && (isAsciiAlnum(v.at(j))
                                || v.at(j) == QLatin1Char('_'))) ++j;
        const auto it = bySymbol.constFind(v.mid(i, j - i));
        if (it != bySymbol.constEnd() && !out.contains(*it)) out.append(*it);
        i = j;
    }
}

// ANTS-3680 — one line against ONE symbol's anchors, split out of scanFile so
// the batch walk can run the same matcher per needle a line carries. `line` is
// the raw decoded line, `thisLine` its trimmed form (computed once per line,
// not once per needle), `prevLine` the previous trimmed line for the wrapped
// pair.
void matchLine(ScanState &st, const Anchors &an, const QString &rel,
               const QString &line, const QString &thisLine,
               const QString &prevLine, int lineNo) {
    bool isDef = false;
    for (const QRegularExpression &re : an.def) {
        if (re.match(line).hasMatch()) { isDef = true; break; }
    }

    // ANTS-4603 — the wrapped-signature pair, tried only once the
    // single-line anchors have all missed. `&&` is excluded here rather
    // than in `wrappedPrev`: a reference return type legitimately ends in
    // a single `&` (`const std::vector<UrlSpan> &`), and telling that from
    // a wrapped boolean is a two-character test, not a regex.
    bool isWrapped = false;
    if (!isDef && an.hasWrapped && !prevLine.isEmpty()
        && !prevLine.endsWith(QLatin1String("&&"))
        && an.wrappedDef.match(line).hasMatch()
        && an.wrappedPrev.match(prevLine).hasMatch()) {
        isDef = true;
        isWrapped = true;
    }

    if (isDef && st.collectDefs) {
        ++st.defsTotal;
        // A wrapped match reports BOTH lines, so the signature carries the
        // return type the query was really about. `line` still names where
        // the symbol is, which is what a reader jumps to.
        const QString trimmed =
            isWrapped ? prevLine + QLatin1Char(' ') + thisLine : thisLine;
        QString kind = QStringLiteral("definition");
        if (an.cppKind) {
            if (trimmed.startsWith(QLatin1String("namespace"))) {
                // ANTS-4346 — neither a body nor a prototype.
                kind = QStringLiteral("namespace");
            } else if (looksLikeDeclaration(trimmed)) {
                kind = QStringLiteral("declaration");
            }
        }
        QVector<DefMatch> &bucket =
            (kind == QStringLiteral("declaration")) ? st.defsDecl : st.defsDefn;
        if (bucket.size() < st.defCap) {
            DefMatch d;
            d.file = rel;
            d.line = lineNo;
            d.signature = trimmed;
            d.lang = QString::fromLatin1(an.lang);
            d.kind = kind;
            bucket.append(d);
        }
    }

    // A definition line is never reported as a caller (INV-9).
    if (st.collectCalls && !isDef) {
        if (an.call.match(line).hasMatch()) {
            ++st.callsTotal;
            if (st.calls.size() < st.callCap) {
                CallMatch c;
                c.file = rel;
                c.line = lineNo;
                c.context = line.trimmed();
                c.lang = QString::fromLatin1(an.lang);
                st.calls.append(c);
            }
        }
    }
}

void scanFile(ScanState &st, const QFileInfo &fi, Lang lang) {
    const int langOrd = static_cast<int>(lang);
    const Anchors &an = st.anchors[langOrd];
    QVarLengthArray<int, 8> hits;
    QFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;  // unopenable → not counted
    ++st.filesScanned;

    const QString rel = fi.absoluteFilePath().mid(st.rootPrefixLen);
    int lineNo = 0;
    QString prevLine;   // ANTS-4603 — previous line, trimmed; "" at file start
    while (!f.atEnd()) {
        QByteArray raw = f.readLine();
        ++lineNo;
        // ANTS-1786 — chop trailing CR/LF and length-check on the RAW
        // bytes before decoding. UTF-8 byte length is exactly what
        // kMaxLineBytes means, so checking the raw size avoids both the
        // per-line throwaway toUtf8() the old code did AND the QString
        // decode for over-long lines that are skipped anyway. Across a
        // multi-thousand-file scan that is millions of avoided allocs.
        while (!raw.isEmpty() && (raw.back() == '\n' || raw.back() == '\r'))
            raw.chop(1);
        if (raw.size() > kMaxLineBytes) {
            // INV-6 skips the line for matching but still advances lineNo.
            // The previous-line context has to be dropped with it: keeping it
            // would let a return type license a qualified call two lines
            // below, across text nothing looked at.
            prevLine.clear();
            continue;                              // backtracking guard
        }
        const QString line = QString::fromUtf8(raw);
        const QString thisLine = line.trimmed();

        // ANTS-3680 — every anchor that consumes the symbol (`def`,
        // `wrappedDef`, `call`) splices its escaped literal into the pattern
        // as mandatory text on THIS line, so a line without that literal can
        // match none of them. A substring test is a sound necessary
        // condition and skips the regex engine on almost every line.
        // `prevLine` still advances either way: the wrapped pair reads it,
        // and a line that is only ever context never carries the symbol.
        if (st.batch) {
            needleSlots(*st.bySymbol, line, hits);
            for (const int idx : hits) {
                ScanState &sl = (*st.batch)[idx];
                matchLine(sl, sl.anchors[langOrd], rel, line, thisLine,
                          prevLine, lineNo);
            }
        } else if (st.symbol.isEmpty() || line.contains(st.symbol)) {
            matchLine(st, an, rel, line, thisLine, prevLine, lineNo);
        }

        prevLine = thisLine;
    }
    f.close();
}

void walk(ScanState &st, const QString &dirPath) {
    if (st.walkCapped) return;
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name);
    for (const QFileInfo &fi : entries) {
        if (st.walkCapped) return;
        if (fi.isSymLink()) continue;  // never follow symlinks out of root

        if (fi.isDir()) {
            const QString name = fi.fileName();
            if (name.startsWith(QLatin1Char('.'))) continue;
            if (name == QLatin1String("build") ||
                name.startsWith(QLatin1String("build-"))) continue;
            if (name == QLatin1String("node_modules")) continue;
            walk(st, fi.absoluteFilePath());
            continue;
        }
        if (!fi.isFile()) continue;

        const Lang lang = langForExt(fi.suffix().toLower());
        if (lang == Lang::Auto) continue;  // unknown extension

        // ANTS-1950 — note a file whose base name equals the query, before
        // the lang filter so a stem hint surfaces even when the filter would
        // otherwise skip the file. Recorded once (first walk-order hit).
        // ANTS-4346 — compare case-INSENSITIVELY. The exact compare could
        // never fire on this codebase's own convention, where a PascalCase
        // symbol (`DocIntegrity`) lives in a lowercase file
        // (`docintegrity.cpp`), so the rescue existed and was unreachable
        // for precisely the queries that most need it.
        if (st.fileStemHit.isEmpty() && !st.symbol.isEmpty()
            && fi.completeBaseName().compare(st.symbol, Qt::CaseInsensitive) == 0)
            st.fileStemHit = fi.absoluteFilePath().mid(st.rootPrefixLen);
        // ANTS-3680 — the same hint per needle, keyed on the lowercased stem
        // so the batch pays one hash lookup per file rather than one compare
        // per needle per file.
        if (st.byStem) {
            const auto it = st.byStem->constFind(fi.completeBaseName().toLower());
            if (it != st.byStem->constEnd()) {
                ScanState &sl = (*st.batch)[*it];
                if (sl.fileStemHit.isEmpty())
                    sl.fileStemHit = fi.absoluteFilePath().mid(st.rootPrefixLen);
            }
        }

        if (st.langFilter != Lang::Auto && lang != st.langFilter) continue;

        if (st.filesScanned >= st.maxFiles) { st.walkCapped = true; return; }
        scanFile(st, fi, lang);
    }
}

void prepare(ScanState &st, const QString &rootCanonical,
             const QString &symbol, const Options &opts) {
    const QString root = QDir::cleanPath(rootCanonical);
    st.rootPrefixLen = root.length() + 1;  // strip "<root>/"
    st.symbol = symbol;                     // ANTS-1950 — stem-hint comparison
    st.langFilter = opts.lang;
    st.maxFiles = (opts.maxFiles > 0) ? opts.maxFiles : kDefaultMaxFiles;

    const QString s = QRegularExpression::escape(symbol);
    auto buildIf = [&](Lang l) {
        if (opts.lang == Lang::Auto || opts.lang == l)
            st.anchors[static_cast<int>(l)] = buildAnchors(l, s);
    };
    buildIf(Lang::Cpp);
    buildIf(Lang::Py);
    buildIf(Lang::Lua);
    buildIf(Lang::Sh);
    buildIf(Lang::Generic);
    buildIf(Lang::Glsl);   // ANTS-3558
}

// Assemble one needle's result: definitions first, then declarations, each
// already in walk (path) order, truncated to the per-needle cap. `filesScanned`
// and `walkCapped` describe the WALK, which the batch shares across needles.
DefResult finishDefs(const ScanState &st, int filesScanned, bool walkCapped) {
    DefResult r;
    r.definitions = st.defsDefn;
    for (const DefMatch &d : st.defsDecl) r.definitions.append(d);
    if (r.definitions.size() > st.defCap) r.definitions.resize(st.defCap);

    r.definitionsTotal = st.defsTotal;
    r.filesScanned = filesScanned;
    r.walkCapped = walkCapped;
    r.truncated = st.defsTotal > r.definitions.size();
    // ANTS-1950 — only a hint when there was nothing to find: a real symbol
    // that also happens to share a file's stem should not be second-guessed.
    if (r.definitions.isEmpty())
        r.fileStemHint = st.fileStemHit;
    return r;
}

bool rootUsable(const QString &rootCanonical) {
    return !rootCanonical.isEmpty() && QFileInfo(rootCanonical).isDir();
}

}  // namespace

Lang parseLang(const QString &s) {
    if (s.isEmpty() || s == QLatin1String("auto")) return Lang::Auto;
    if (s == QLatin1String("cpp")) return Lang::Cpp;
    if (s == QLatin1String("py"))  return Lang::Py;
    if (s == QLatin1String("lua")) return Lang::Lua;
    if (s == QLatin1String("sh"))  return Lang::Sh;
    if (s == QLatin1String("generic")) return Lang::Generic;
    if (s == QLatin1String("glsl")) return Lang::Glsl;   // ANTS-3558
    return Lang::Auto;
}

bool isValidSymbol(const QString &symbol) {
    if (symbol.isEmpty() || symbol.size() > kSymbolMaxLen) return false;
    const QChar c0 = symbol.at(0);
    if (!isAsciiAlpha(c0) && c0 != QLatin1Char('_')) return false;
    for (int i = 1; i < symbol.size(); ++i) {
        const QChar c = symbol.at(i);
        if (!isAsciiAlnum(c) && c != QLatin1Char('_')) return false;
    }
    return true;
}

DefResult findDefinition(const QString &rootCanonical,
                         const QString &symbol,
                         const Options &opts) {
    DefResult r;
    if (!isValidSymbol(symbol)) {
        r.ok = false;
        r.code = QStringLiteral("bad_args");
        r.error = QStringLiteral("find_definition: invalid symbol");
        return r;
    }
    if (!rootUsable(rootCanonical)) return r;  // empty ok result

    ScanState st;
    prepare(st, rootCanonical, symbol, opts);
    st.collectDefs = true;
    st.defCap = (opts.maxResults > 0) ? opts.maxResults : kDefDefaultResults;

    walk(st, QDir::cleanPath(rootCanonical));
    return finishDefs(st, st.filesScanned, st.walkCapped);
}

// ANTS-3680 — N needles in ONE walk. Equivalent to calling findDefinition
// once per symbol: same anchors, same caps, same ordering. What it removes is
// the per-needle re-walk — measured at ~81 ms a needle on this tree after the
// per-line prefilter, of which ~45 ms was re-reading files already read.
//
// Caps stay per-needle (`maxResults` bounds each result, not the batch) so one
// popular name cannot eat the budget; `maxFiles` is per-WALK, which is what it
// already meant. RAM is one file's lines at a time, as before — nothing is
// cached across files.
QHash<QString, DefResult> findDefinitions(const QString &rootCanonical,
                                          const QStringList &symbols,
                                          const Options &opts) {
    QHash<QString, DefResult> out;
    QStringList needles;                 // stable: `bySymbol` views into it
    needles.reserve(symbols.size());
    for (const QString &sym : symbols) {
        if (out.contains(sym)) continue;             // duplicate needle
        if (!isValidSymbol(sym)) {
            DefResult bad;
            bad.ok = false;
            bad.code = QStringLiteral("bad_args");
            bad.error = QStringLiteral("find_definition: invalid symbol");
            out.insert(sym, bad);
            continue;
        }
        out.insert(sym, DefResult{});
        needles << sym;
    }
    if (needles.isEmpty() || !rootUsable(rootCanonical)) return out;

    QVector<ScanState> batchSlots(needles.size());
    QHash<QStringView, int> bySymbol;
    QHash<QString, int> byStem;
    for (int i = 0; i < needles.size(); ++i) {
        prepare(batchSlots[i], rootCanonical, needles.at(i), opts);
        batchSlots[i].collectDefs = true;
        batchSlots[i].defCap = (opts.maxResults > 0) ? opts.maxResults
                                                : kDefDefaultResults;
        bySymbol.insert(QStringView(needles.at(i)), i);
        byStem.insert(needles.at(i).toLower(), i);
    }

    // The walk state carries no symbol of its own; it owns the file budget.
    ScanState walkSt;
    prepare(walkSt, rootCanonical, QString(), opts);
    walkSt.batch = &batchSlots;
    walkSt.bySymbol = &bySymbol;
    walkSt.byStem = &byStem;
    walk(walkSt, QDir::cleanPath(rootCanonical));

    for (int i = 0; i < needles.size(); ++i)
        out[needles.at(i)] =
            finishDefs(batchSlots[i], walkSt.filesScanned, walkSt.walkCapped);
    return out;
}

CallResult findCaller(const QString &rootCanonical,
                      const QString &symbol,
                      const Options &opts) {
    CallResult r;
    if (!isValidSymbol(symbol)) {
        r.ok = false;
        r.code = QStringLiteral("bad_args");
        r.error = QStringLiteral("find_caller: invalid symbol");
        return r;
    }
    if (!rootUsable(rootCanonical)) return r;  // empty ok result

    ScanState st;
    prepare(st, rootCanonical, symbol, opts);
    st.collectCalls = true;
    st.callCap = (opts.maxResults > 0) ? opts.maxResults : kCallDefaultResults;
    // Also collect definitions (a small bucket) so we can attach the
    // best def for the "where + who" round trip.
    st.collectDefs = true;
    st.defCap = kDefDefaultResults;

    // ANTS-3805 — scope the walk to `lane` when given. Cleaned and re-checked
    // against the root so a `..` cannot walk outside the project; a lane that
    // does not resolve under the root, or does not exist, is ignored rather
    // than refused — the answer is then the whole-project scan the caller would
    // have got anyway, never a silent empty result that reads as "no callers".
    QString scanRoot = QDir::cleanPath(rootCanonical);
    if (!opts.lane.isEmpty()) {
        const QString cand =
            QDir::cleanPath(scanRoot + QLatin1Char('/') + opts.lane);
        if ((cand == scanRoot || cand.startsWith(scanRoot + QLatin1Char('/'))) &&
            QFileInfo(cand).isDir())
            scanRoot = cand;
    }
    walk(st, scanRoot);

    r.callers = st.calls;
    r.callersTotal = st.callsTotal;
    r.filesScanned = st.filesScanned;
    r.walkCapped = st.walkCapped;
    r.truncated = st.callsTotal > r.callers.size();

    // Best def: first definition, else first declaration (walk order).
    if (!st.defsDefn.isEmpty())      r.definition = st.defsDefn.first();
    else if (!st.defsDecl.isEmpty()) r.definition = st.defsDecl.first();

    return r;
}

}  // namespace SymbolQuery
