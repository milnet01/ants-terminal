// ANTS-1249 — file_outline regex scanner. See fileoutline.h header.

#include "fileoutline.h"

#include "markdownscan.h"   // ANTS-4520 — shared blockquote strip

#include <QSet>
#include <QHash>
#include <QPair>
#include <QVector>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QString>

namespace FileOutline {

// ANTS-3800 — the SAME extension set symbolquery.cpp's ANTS-3558 lane accepts,
// deliberately: the whole defect was three verbs disagreeing about which files
// are GLSL, and a second list here would be that defect with an extra step.
// Note what it omits, and why: `.vs`/`.fs`/`.gs` are excluded because `.fs` is
// also F# source.
// ANTS-4096 — hoisted out of the anonymous namespace and declared in the
// header so CodebaseIndex::isIndexableSuffix reuses this list instead of
// growing the fourth copy the comment above warns about.
bool isGlslExt(const QString &ext) {
    static const QSet<QString> kGlsl = {
        QStringLiteral("glsl"), QStringLiteral("comp"), QStringLiteral("frag"),
        QStringLiteral("vert"), QStringLiteral("geom"), QStringLiteral("tesc"),
        QStringLiteral("tese"), QStringLiteral("vsh"),  QStringLiteral("fsh"),
        QStringLiteral("mesh"), QStringLiteral("task"), QStringLiteral("rgen"),
        QStringLiteral("rchit"),QStringLiteral("rmiss"),QStringLiteral("rahit"),
        QStringLiteral("rint"), QStringLiteral("rcall"),
    };
    return kGlsl.contains(ext);
}

// ANTS-4425 — see the header. `.htm` is the DOS-era spelling of the same thing
// and pickModeByExt has always accepted both.
bool isHtmlExt(const QString &ext) {
    return ext == QLatin1String("html") || ext == QLatin1String("htm");
}

namespace {

constexpr int kMaxLineBytes      = 1024;   // ANTS-1249-INV-8
constexpr int kHeaderDocByteCap  = 2048;   // ANTS-1249-INV-9
constexpr int kHeaderDocMaxLines = 30;

// Regex set — `static const`, `.optimize()` called once (Qt does this
// implicitly on first match call after construction). PCRE2 JIT is
// enabled at the QRegularExpression compile call. Possessive
// quantifiers (`+` after `+`) bound backtracking — INV-8.
//
// ANTS-1249-INV-8: regex set compiled once per process. The QtMagic
// init pattern (`Q_GLOBAL_STATIC` would be heavier here) — block-
// scope `static const` initialises on first use and survives the
// process.

const QRegularExpression &rxCppMember() {
    static const QRegularExpression rx = []{
        // ANTS-3433 — the return type is one-or-more `[\w:<>]+` tokens each
        // followed by a real separator `[\s*&]+` (the same shape as
        // rxCppFunc's ANTS-2028 group). The old single `[\w:]+` return-type
        // token dropped every out-of-line member whose return type is a
        // space-separated builtin — `unsigned int`, `long long`, `unsigned
        // char`, `const T&` — because the second word had no `::` and the
        // qualified-name matcher never reached the real `Class::method`.
        // The leading keyword lookahead rejects a col-0 `return Foo::bar(`
        // statement (mirrors rxCppFunc's ANTS-2147 guard); the qualified
        // name still starts at the run after the last return-type token.
        QRegularExpression r(QStringLiteral(R"(^(?!(?:return|co_return|co_await|co_yield|throw|else)\b)(?:(?![\w:<>]+\s*\()[\w:<>]+[\s*&]+)++[\w:]+::[\w~]+\s*\()"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxCppType() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(class|struct|namespace)\s+(\w+))"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2228 — opening line of the dominant C aggregate idiom
// `typedef struct [TAG] {` (brace opens on this line). Group 1 = optional
// struct tag (absent for an anonymous `typedef struct { … } ALIAS;`). The
// alias sits on the matching close line, captured by rxCppTypedefStructClose.
// Distinct from rxCppType, which only catches a bare `struct X` / forward decl.
const QRegularExpression &rxCppTypedefStructOpen() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^typedef\s+struct\s+(\w+)?\s*\{)"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2228 — `} ALIAS;` (or `} ALIAS, *PTR;`) closing line of a
// typedef-struct; group 1 = the typedef alias.
const QRegularExpression &rxCppTypedefStructClose() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(\}\s*(\w+)\s*[;,])"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxCppFunc() {
    // Possessive `++` quantifiers bound backtracking on adversarial
    // input. The PCRE2 engine emits a single linear scan.
    //
    // ANTS-2028: the return type is one-or-more `[\w:<>]+` tokens, each
    // followed by a real separator `[\s*&]+`. The earlier
    // `[\w:<>&*\s]++\s++(\w+)` folded the return type AND the name into
    // one possessive class, so for `int alpha()` it consumed "int alpha"
    // and the trailing `\s++(\w+)` had nothing left (no backtrack) —
    // free functions never matched. Splitting the return-type tokens
    // from the name capture leaves `(\w+)` an identifier to grab. The
    // inner classes are disjoint (word/colon/angle vs space/star/amp),
    // so the scan stays linear (INV-8).
    //
    // ANTS-2147: a statement-position call (`return foo(...)`,
    // `throw foo(...)`, `else foo(...)` …) has a leading keyword + space
    // that the return-type group `(?:(?![\w:<>]+\s*\()[\w:<>]+[\s*&]+)++` would otherwise
    // absorb, emitting the call site as a spurious function symbol. The
    // negative lookahead rejects an expression-introducing reserved
    // keyword as the first token — a keyword is never a return type, so
    // no real definition is lost. Mirrors the symbolquery.cpp guard.
    //
    // ANTS-3351: an optional leading `extern "C"` (or `extern "C++"`)
    // linkage specifier is consumed by the non-capturing prefix. The `"C"`
    // string literal is not a `[\w:<>]` token, so without this the
    // return-type group stopped after `extern ` and the name capture
    // failed — every `extern "C"` function definition went undetected,
    // and (worse) its body's most-vexing-parse locals leaked as file-scope
    // funcs because the enclosing function never opened a scope. DOOM's
    // r_vulkan.cpp (RB_VulkanProbe + the RB_Vulkan_* entry points). The
    // prefix is shared by rxCppFuncOpen / rxCppFuncHeaderOpen.
    //
    // ANTS-3412: two coupled arg-list / tail defects (Vestige job_system.h).
    //  (a) the old `\([^)]*\)` arg matcher closed on the FIRST ')', so a
    //      parameter whose type carries an inner paren pair — the empty
    //      `std::function<void()>` or the populated `std::function<void(
    //      uint32_t,uint32_t)>` — ended the arg list early and the trailing
    //      `>` broke the tail, dropping the whole method. Replaced with a
    //      one-level-nested paren matcher `\((?:[^()]++|\([^()]*+\))*+\)`
    //      (all-possessive → still a single linear scan, INV-8), which
    //      balances one level of parens inside the declarator list.
    //  (b) the old `\)\s*[{;]` tail forbade any qualifier between ')' and
    //      the body, so an inline `T f() const { … }` accessor never
    //      matched. An optional cv/ref/noexcept/spec qualifier run is now
    //      allowed before the `[{;]` terminator. The qualifier group is
    //      shared verbatim with rxCppFuncOpen.
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(?:extern\s*"[^"]*"\s*)?(static|inline|template[^>]*>)?\s*(?!(?:return|co_return|co_await|co_yield|throw|else)\b)(?:(?![\w:<>]+\s*\()[\w:<>]+[\s*&]+)++(\w+)\s*\((?:[^()]++|\([^()]*+\))*+\)(?:\s*(?:const|volatile|noexcept\s*\((?:[^()]++|\([^()]*+\))*+\)|noexcept|override|final|mutable|&&|&|=\s*(?:0|default|delete)))*+\s*[{;])"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxCppQt() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(signals|slots|public|private|protected|Q_(?:PROPERTY|INVOKABLE|DECLARE_\w+))\s*[:(])"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2159 — multi-line definition support. `rxCppFuncOpen` is rxCppFunc
// with the trailing terminator relaxed to end-of-line: it matches a
// `ReturnType name(args)` header whose body `{` sits on the NEXT line
// (id-Software / GNU brace style). Control keywords are rejected up front.
const QRegularExpression &rxCppFuncOpen() {
    static const QRegularExpression rx = []{
        // ANTS-3412 — same nested-paren arg matcher + tail-qualifier run as
        // rxCppFunc, with the terminator relaxed to end-of-line (body `{` on
        // the next line). So a `ReturnType name(std::function<void()> cb) const`
        // whose brace sits below is still detected.
        QRegularExpression r(QStringLiteral(R"(^(?:extern\s*"[^"]*"\s*)?(static|inline|template[^>]*>)?\s*(?!(?:return|co_return|co_await|co_yield|throw|else|if|for|while|switch|do|catch)\b)(?:(?![\w:<>]+\s*\()[\w:<>]+[\s*&]+)++(\w+)\s*\((?:[^()]++|\([^()]*+\))*+\)(?:\s*(?:const|volatile|noexcept\s*\((?:[^()]++|\([^()]*+\))*+\)|noexcept|override|final|mutable|&&|&|=\s*(?:0|default|delete)))*+\s*$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2159 — a bare `name(args)` (no return type, no terminator) line: the
// continuation of an old-style definition whose return type was on the
// PREVIOUS line (paired with a pending-return-type line by the scanner).
const QRegularExpression &rxCppNameArgs() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^\s*(?!(?:return|co_return|co_await|co_yield|throw|else|if|for|while|switch|do|catch|sizeof|new|delete)\b)([A-Za-z_]\w*(?:::[\w~]+)?)\s*\([^)]*\)\s*$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2148 follow-up — the OPENING line of a function whose parameter list
// wraps across source lines: `ReturnType name(` with args that do NOT close on
// this line (the trailing `[^)]*$` forbids a ')' to end-of-line). rxCppFuncOpen
// only matched a closed `(args)`; this catches id-Software / K&R prototypes
// like `static void emit_wall(builder_t* bld, seg_t* seg, ...,` whose ')' sits
// 2-3 lines down. Control keywords are rejected up front as in the siblings.
const QRegularExpression &rxCppFuncHeaderOpen() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(?:extern\s*"[^"]*"\s*)?(static|inline|template[^>]*>)?\s*(?!(?:return|co_return|co_await|co_yield|throw|else|if|for|while|switch|do|catch)\b)(?:(?![\w:<>]+\s*\()[\w:<>]+[\s*&]+)++(\w+)\s*\([^)]*$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2159 — a line that is ONLY return-type / modifier tokens (no parens,
// terminator, brace or `=`): a candidate return type for an old-style
// definition split across lines. Statement / declaration keywords are
// rejected so a bare `return`/`else`/`class` line is never a pending type.
const QRegularExpression &rxCppTypeOnly() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^\s*(?!(?:return|co_return|else|do|case|default|break|continue|goto|public|private|protected|using|namespace|typedef|friend|template|class|struct|enum|union)\b)[\w:<>*&]+(?:\s+[\w:<>*&]+)*\s*$)"));
        r.optimize();
        return r;
    }();
    return rx;
}
// ANTS-2159 — net `{` minus `}` on a line, ignoring braces inside string /
// char literals, line comments, and block comments, so the scope counter
// doesn't drift on `"{"` or `// {`. `inBlock` carries block-comment state
// across lines. A heuristic (raw-string literals are not special-cased —
// rare in a declaration region); good enough to keep the depth honest.
// ANTS-3735 — `lastCodeIdxOut` optionally reports the index of the line's
// last non-space CODE character (comments excluded), which is what the
// declaration-vs-definition terminator test needs.
int netBraceDelta(const QString &line, bool &inBlock, int *parenDeltaOut = nullptr,
                  int *lastCodeIdxOut = nullptr) {
    int delta = 0;
    if (parenDeltaOut) *parenDeltaOut = 0;
    if (lastCodeIdxOut) *lastCodeIdxOut = -1;
    const int n = line.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = line.at(i);
        if (inBlock) {
            if (c == QLatin1Char('*') && i + 1 < n
                && line.at(i + 1) == QLatin1Char('/')) { inBlock = false; ++i; }
            continue;
        }
        if (c == QLatin1Char('/') && i + 1 < n) {
            const QChar d = line.at(i + 1);
            if (d == QLatin1Char('/')) break;                       // line comment
            if (d == QLatin1Char('*')) { inBlock = true; ++i; continue; }
        }
        // ANTS-3735 — this character is CODE (past the comment tests above).
        // Record it so the caller can test a line's real terminator without a
        // trailing comment hiding it: `int f(void);   // note` ends in ';',
        // but QString::endsWith(';') on the raw line says otherwise, which
        // read as "this definition opens a body" and latched the scanner
        // inside a phantom function for the rest of the file.
        if (lastCodeIdxOut && !c.isSpace()) *lastCodeIdxOut = i;
        if (c == QLatin1Char('"')) {
            // Raw string R"delim( … )delim" (L/u8/u/U prefix keyed on the
            // immediately-preceding 'R'): its content — incl. braces in a
            // regex literal like R"({1,6})" — is fully literal and must not
            // move the depth. Critical for self-scan (this codebase's regex
            // builders sit inside function bodies).
            if (i > 0 && line.at(i - 1) == QLatin1Char('R')) {
                int j = i + 1;
                QString delim;
                while (j < n && line.at(j) != QLatin1Char('(')) {
                    delim.append(line.at(j)); ++j;
                }
                const QString close =
                    QStringLiteral(")") + delim + QStringLiteral("\"");
                const int end = (j < n) ? line.indexOf(close, j) : -1;
                if (end < 0) { i = n; break; }   // unterminated on this line
                i = end + close.size() - 1;
                if (lastCodeIdxOut) *lastCodeIdxOut = i;
                continue;
            }
            for (++i; i < n; ++i) {              // ordinary string literal
                const QChar e = line.at(i);
                if (e == QLatin1Char('\\')) { ++i; continue; }
                if (e == QLatin1Char('"')) break;
            }
            if (lastCodeIdxOut) *lastCodeIdxOut = qMin(i, n - 1);
            continue;
        }
        if (c == QLatin1Char('\'')) {            // char literal
            for (++i; i < n; ++i) {
                const QChar e = line.at(i);
                if (e == QLatin1Char('\\')) { ++i; continue; }
                if (e == QLatin1Char('\'')) break;
            }
            if (lastCodeIdxOut) *lastCodeIdxOut = qMin(i, n - 1);
            continue;
        }
        if (c == QLatin1Char('{')) ++delta;
        else if (c == QLatin1Char('}')) --delta;
        else if (parenDeltaOut) {
            // ANTS-2148 follow-up — the same literal/comment-aware pass tallies
            // parens so the wrapped-parameter-list collector shares one scan
            // (no second pass, no double-toggle of the block-comment state).
            if (c == QLatin1Char('(')) ++*parenDeltaOut;
            else if (c == QLatin1Char(')')) --*parenDeltaOut;
        }
    }
    return delta;
}
// ANTS-4379 — a TOP-LEVEL Python assignment (`NAME = …`, `NAME: T = …`).
// The brace family has carried a `const` kind since ANTS-4090; Python had no
// equivalent, and doc_symbols shares this index — so a spec citing a
// module-level constant read as a broken reference.
//
// It bites a specific and unavoidable shape: a codebase whose user-facing
// error strings are module constants, which is the natural Python idiom and
// exactly what a spec ABOUT error messages must cite.
//
// Anchored at column 0 — an indented assignment is a local or a class
// attribute, not a module constant, and emitting those would bury the outline
// in every intermediate variable in every function body.
// ANTS-4361 — an element's `id="…"` attribute: the anchor a caller navigates
// to. Single or double quoted, since a hand-written page uses both.
const QRegularExpression &rxHtmlId() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(
            R"(\bid\s*=\s*["\']([A-Za-z_][\w:.-]*)["\'])"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxPyConst() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(
            R"(^([A-Za-z_]\w*)\s*(?::[^=]+)?=(?!=))"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxPy() {
    // ANTS-3404 — capture leading indentation (group 1) so an indented
    // `def`/`class` is matched too, not only a top-level `^def`. compute()
    // uses the indent to qualify a class method as `Class.method` (so
    // read_region symbol-mode can address it). Groups: 1=indent,
    // 2=async?, 3=keyword, 4=name.
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(\s*)(async\s+)?(def|class)\s+(\w+))"));
        r.optimize();
        return r;
    }();
    return rx;
}
const QRegularExpression &rxMdHeading() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(#{1,6})\s+(.+)$)"));
        r.optimize();
        return r;
    }();
    return rx;
}

// ANTS-2150 — brace-family generic outline (Mode::Generic). Three regexes
// cover the top-level declaration surface shared across Rust / Go / JS / TS /
// Java / C# / Kotlin / Swift / Scala / PHP. Possessive quantifiers (`*+`,
// `++`) + the per-line byte cap bound backtracking (INV-8), same as the C++ set.
// Ruby (`.rb`) folds in here too: `def`/`class`/`module` are already in the
// keyword alternation, and `end`-blocks are irrelevant to a line-oriented
// outline. rxGenericDecl carries an optional `self.` receiver (`def self.x`
// singleton methods) and an optional trailing `?`/`!` (Ruby predicate/bang
// names) — both no-ops for the brace-family languages. Ruby's `#`-comment
// header_doc is not extracted (Mode::Generic's marker is `//`); symbols still
// extract fully.

// (1) keyword declarations: optional modifiers, a declaration keyword, an
// optional Go receiver `(s *T)`, then the name. Captures kw=group1, name=group2.
const QRegularExpression &rxGenericDecl() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^\s*(?:(?:pub|export|default|public|private|protected|internal|static|final|abstract|sealed|async|open|override|suspend|inline|const|unsafe|extern|data)\s++)*+(fn|fun|func|function|def|class|struct|enum|trait|impl|interface|type|module|object|protocol|extension|namespace|record)\b(?:\s++\([^)]*\))?\s++(?:self\.)?([A-Za-z_$][\w$]*[?!]?))"));
        r.optimize();
        return r;
    }();
    return rx;
}
// (2) C-style method definitions (Java / C# / Swift / Kotlin members that lack
// a leading decl keyword): return-type token(s) + name + parens + a body `{`.
// The trailing `{` (not `;`) keeps it a definition, never a call; the negative
// lookahead rejects control-flow keywords. Captures name=group1.
const QRegularExpression &rxGenericMethod() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^\s*(?!(?:return|if|for|while|switch|catch|else|throw|new|await|do|in|of)\b)(?:[A-Za-z_$<>\[\].]+[\s*&]++)++([A-Za-z_$][\w$]*)\s*\([^;{]*\)\s*(?:->\s*[\w$<>\[\].?]+\s*|:\s*[\w$<>\[\].?]+\s*)?\{)"));
        r.optimize();
        return r;
    }();
    return rx;
}
// (3) JS/TS arrow-function assignments: `export const foo = (…) => …`.
// Captures name=group1.
const QRegularExpression &rxGenericArrow() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^\s*(?:export\s++)?(?:default\s++)?(?:const|let|var)\s++([A-Za-z_$][\w$]*)\s*=\s*(?:async\s++)?(?:\([^)]*\)|[A-Za-z_$][\w$]*)\s*=>)"));
        r.optimize();
        return r;
    }();
    return rx;
}

// (4) ANTS-4090 — top-level data bindings: `const NAME =`, `let`, `var`, with
// or without `export`. Rule (3) only catches the arrow-function form, and
// rxGenericDecl lists `const` as a MODIFIER before a declaration keyword, so a
// binding holding a template literal, object or string matched nothing — and in
// a file that stores payloads that way those are its largest regions.
//
// Anchored at column 0 with NO leading-whitespace class (unlike rules 1–3):
// indentation survives to the match here, and it is the only cheap signal a
// line-based scanner has for "top level". Without it every local inside every
// function body would be outlined. Captures name=group1.
const QRegularExpression &rxGenericBinding() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(?:export\s++)?(?:default\s++)?(?:const|let|var)\s++([A-Za-z_$][\w$]*)\s*=)"));
        r.optimize();
        return r;
    }();
    return rx;
}

// Map a brace-family extension to its precise language name for the response
// `language` field (so the map reads "rust"/"typescript", not "generic").
// Returns "" for an extension not in the generic family.
QString genericLangName(const QString &ext) {
    if (ext == QLatin1String("rs"))   return QStringLiteral("rust");
    if (ext == QLatin1String("go"))   return QStringLiteral("go");
    if (ext == QLatin1String("js") || ext == QLatin1String("jsx") ||
        ext == QLatin1String("mjs") || ext == QLatin1String("cjs"))
        return QStringLiteral("javascript");
    if (ext == QLatin1String("ts") || ext == QLatin1String("tsx"))
        return QStringLiteral("typescript");
    if (ext == QLatin1String("java"))  return QStringLiteral("java");
    if (ext == QLatin1String("cs"))    return QStringLiteral("csharp");
    if (ext == QLatin1String("kt") || ext == QLatin1String("kts"))
        return QStringLiteral("kotlin");
    if (ext == QLatin1String("swift")) return QStringLiteral("swift");
    if (ext == QLatin1String("scala") || ext == QLatin1String("sc"))
        return QStringLiteral("scala");
    if (ext == QLatin1String("php"))   return QStringLiteral("php");
    if (ext == QLatin1String("rb"))    return QStringLiteral("ruby");  // ANTS-2150
    return QString();
}

bool isGenericExt(const QString &ext) { return !genericLangName(ext).isEmpty(); }

Mode pickModeByExt(const QString &absPath) {
    const QString ext = QFileInfo(absPath).suffix().toLower();
    // ANTS-2148 — the C family (`.c`, `.hxx`) outlines fine with the C++
    // regex set (shared surface syntax); admitting it here keeps
    // codebase_index (isIndexableSuffix) able to extract symbols from a
    // C-only project instead of returning an empty map.
    if (ext == QLatin1String("cpp") || ext == QLatin1String("cc")  ||
        ext == QLatin1String("cxx") || ext == QLatin1String("c")   ||
        ext == QLatin1String("h")   || ext == QLatin1String("hpp") ||
        ext == QLatin1String("hh")  || ext == QLatin1String("hxx")) {
        return Mode::Cpp;
    }
    if (ext == QLatin1String("py"))  return Mode::Py;
    if (ext == QLatin1String("md")   || ext == QLatin1String("markdown") ||
        ext == QLatin1String("txt")) return Mode::Md;
    if (ext == QLatin1String("json")) return Mode::Json;
    if (isGlslExt(ext)) return Mode::Glsl;        // ANTS-3800 shader stages
    if (isHtmlExt(ext)) return Mode::Html;       // ANTS-4361 / ANTS-4425
    if (isGenericExt(ext)) return Mode::Generic;  // ANTS-2150 brace family
    return Mode::Auto;  // sentinel meaning "unknown" downstream
}

const char *modeToLanguageString(Mode m) {
    switch (m) {
        case Mode::Cpp:     return "cpp";
        case Mode::Py:      return "py";
        case Mode::Md:      return "md";
        case Mode::Json:    return "json";
        case Mode::Generic: return "generic";  // compute() overrides w/ precise name
        case Mode::Html:    return "html";     // ANTS-4361
        case Mode::Glsl:    return "glsl";     // ANTS-3800
        case Mode::Auto:    return "unknown";
    }
    return "unknown";
}

QString headerCommentMarker(Mode m) {
    switch (m) {
        case Mode::Cpp:     return QStringLiteral("//");
        case Mode::Generic: return QStringLiteral("//");  // brace family (ANTS-2150)
        case Mode::Html:    return QStringLiteral("<!--"); // ANTS-4361
        case Mode::Glsl:    return QStringLiteral("//");  // ANTS-3800
        case Mode::Py:      return QStringLiteral("#");
        case Mode::Md:      return QStringLiteral("<!--");
        default:            return QString();
    }
}

void appendSymbol(QJsonArray &arr,
                  int line,
                  const char *kind,
                  const QString &name,
                  const QString &signature) {
    QJsonObject s;
    s["line"]      = line;
    s["kind"]      = QString::fromLatin1(kind);
    s["name"]      = name;
    s["signature"] = signature;
    arr.append(s);
}

QJsonObject errObj(const char *code, const QString &message) {
    QJsonObject o;
    o["ok"]    = false;
    o["error"] = message;
    o["code"]  = QString::fromLatin1(code);
    return o;
}

}  // namespace

Mode parseMode(const QString &s) {
    if (s.isEmpty() || s == QLatin1String("auto")) return Mode::Auto;
    if (s == QLatin1String("cpp"))  return Mode::Cpp;
    if (s == QLatin1String("py"))   return Mode::Py;
    if (s == QLatin1String("md"))   return Mode::Md;
    if (s == QLatin1String("html")) return Mode::Html;   // ANTS-4361
    if (s == QLatin1String("json")) return Mode::Json;
    if (s == QLatin1String("generic")) return Mode::Generic;
    if (s == QLatin1String("glsl")) return Mode::Glsl;   // ANTS-3800
    return Mode::Auto;
}

QJsonObject compute(const QString &absPath,
                    Mode mode,
                    bool includeDocComment,
                    int maxSymbols,
                    bool withSizes,
                    int maxHeadingLevel) {
    if (maxSymbols <= 0)            maxSymbols = 200;
    if (maxSymbols > kMaxSymbolsCap) maxSymbols = kMaxSymbolsCap;

    // ANTS-1249-INV-2: existence + access check. The path-escape
    // guard lives at the call site (cmdFileOutline) so this function
    // can be unit-tested without a project-root harness.
    QFile f(absPath);
    if (!f.exists()) {
        return errObj("not_found",
            QStringLiteral("file_outline: \"%1\" does not exist").arg(absPath));
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return errObj("not_found",
            QStringLiteral("file_outline: cannot open \"%1\"").arg(absPath));
    }

    // Resolve effective mode (auto → ext-based).
    Mode effective = (mode == Mode::Auto) ? pickModeByExt(absPath) : mode;
    // Mode::Auto still here = unknown extension; treat as "unknown".
    const bool isUnknown = (effective == Mode::Auto);

    // ANTS-1249-INV-6: header_doc starts empty (not null).
    QString headerDoc;
    QJsonArray symbols;
    int totalLines = 0;
    qint64 totalBytes = 0;
    // ANTS-4384 — sizing state, built only when asked for.
    // lineStart[i] = byte offset of line i+1; a sentinel totalBytes is
    // appended after the walk so the last symbol's extent needs no special
    // case. mdLevelByLine carries heading depth for the ONE mode that has
    // levels; every other mode is flat, so its symbols are siblings and each
    // runs to the next.
    QVector<qint64> lineStart;
    QHash<int, int> mdLevelByLine;
    int seenSymbols = 0;
    bool truncated = false;
    bool inHeaderBlock = includeDocComment && !isUnknown &&
                         effective != Mode::Json;
    const QString headerMarker = headerCommentMarker(effective);
    int headerLinesEmitted = 0;

    // ANTS-2159 — C++ scope tracking: a function/member symbol is emitted
    // only at file or type-body scope, never inside a code body (so a
    // most-vexing-parse local or a `case X: return f();` statement is not
    // mistaken for a function); and a definition whose return type and/or
    // `{` sit on adjacent lines is still found.
    int braceDepth = 0;            // literal/comment-aware net brace depth
    int funcOpenAtDepth = -1;      // depth a function body opened at; -1 = file/type scope
    bool funcBodyEntered = false;  // the body's opening '{' has been seen
    bool inBlockComment = false;   // block-comment carry for netBraceDelta
    QString pendingType;           // prior file-scope line that was a bare return type
    // ANTS-2148 follow-up — wrapped-parameter-list collector state. When a
    // function header opens '(' without closing it on the same line, collect
    // continuation lines until the parens balance, then emit at the header's
    // start line so read_region symbol-mode resolves the whole definition.
    bool inFuncArgs = false;
    int funcArgParenDepth = 0;      // running '(' - ')' across the wrapped header
    int funcArgStartLine = 0;       // 1-based line where the name sits
    QString funcArgName;           // captured function name
    QString funcArgSig;            // accumulated header text → signature
    // ANTS-2228 — typedef-struct capture. A `typedef struct TAG { … } ALIAS;`
    // opens here and emits at its START line when the brace balances back,
    // keyed by BOTH the tag and the alias so read_region symbol-mode (and
    // ANTS-2222's aggregate-body slice) resolve either.
    struct PendingTypedef { int startLine{}; QString tag; int depthAtOpen{}; };
    QVector<PendingTypedef> pendingTypedefs;
    // ANTS-3404 — Python class-method qualification. Track enclosing
    // classes by indentation so an indented `def` is emitted as
    // `Class.method` (read_region symbol-mode can then address it); a
    // top-level def/class stays bare. Each entry = (indent width, dotted
    // class-name prefix so far). Nested classes chain (`Outer.Inner`).
    QVector<QPair<int, QString>> pyClassStack;
    // ANTS-4722 — Md fence state. Null when outside a fenced block, else the
    // fence character (backtick or tilde) that opened the one we are in.
    QChar mdFenceChar;
    int   mdFenceRun = 0;  // ANTS-4820 — the open fence's run length
    // ANTS-4361 — HTML landmark state.
    bool inHtmlScript  = false;
    bool htmlScriptIsJs = true;

    while (!f.atEnd()) {
        const QByteArray rawLine = f.readLine();
        if (withSizes) lineStart.append(totalBytes);
        totalBytes += rawLine.size();
        ++totalLines;

        // Strip CRLF / LF for matching but keep the raw byte count.
        QByteArray trimmed = rawLine;
        while (!trimmed.isEmpty() &&
               (trimmed.back() == '\n' || trimmed.back() == '\r')) {
            trimmed.chop(1);
        }

        // Header doc collector — runs only for the leading contiguous
        // comment block. Stops at first non-comment line.
        if (inHeaderBlock) {
            const QString line = QString::fromUtf8(trimmed);
            const QString stripped = line.trimmed();
            bool isComment = false;
            if (effective == Mode::Md) {
                // Markdown: collect a leading HTML-comment block or
                // the first run of non-heading prose. We use a tiny
                // heuristic — any line up to the first blank or first
                // heading counts as the file lead.
                if (stripped.startsWith(QStringLiteral("<!--")) ||
                    (headerLinesEmitted == 0 &&
                     !stripped.startsWith(QLatin1Char('#')) &&
                     !stripped.isEmpty())) {
                    isComment = true;
                } else if (stripped.isEmpty() && headerLinesEmitted == 0) {
                    // Allow one leading blank line.
                    isComment = false;
                    inHeaderBlock = false;
                } else {
                    inHeaderBlock = false;
                }
            } else {
                if (stripped.isEmpty() && headerLinesEmitted == 0) {
                    // Leading blank line — keep scanning.
                    isComment = false;
                } else if (!headerMarker.isEmpty() &&
                           stripped.startsWith(headerMarker)) {
                    isComment = true;
                } else {
                    inHeaderBlock = false;
                }
            }
            if (isComment && headerLinesEmitted < kHeaderDocMaxLines) {
                if (!headerDoc.isEmpty()) headerDoc.append(QLatin1Char('\n'));
                headerDoc.append(line);
                ++headerLinesEmitted;
                if (headerDoc.toUtf8().size() > kHeaderDocByteCap) {
                    // ANTS-1249-INV-9: 2 KiB header_doc cap.
                    headerDoc.truncate(kHeaderDocByteCap);
                    headerDoc.append(QChar(0x2026));  // …
                    inHeaderBlock = false;
                }
            }
            if (headerLinesEmitted >= kHeaderDocMaxLines) inHeaderBlock = false;
        }

        // ANTS-1249-INV-8: skip regex scan on lines exceeding the
        // per-line byte cap (catastrophic-backtracking guard).
        if (trimmed.size() > kMaxLineBytes) continue;
        if (isUnknown) continue;

        const QString line = QString::fromUtf8(trimmed);

        // Symbols cap check. Increment `seenSymbols` only when a regex
        // would have matched, so `truncated` is accurate per INV-5.
        auto offer = [&](const char *kind,
                          const QString &name,
                          const QString &signature) {
            ++seenSymbols;
            if (symbols.size() >= maxSymbols) {
                truncated = true;
                return;
            }
            appendSymbol(symbols, totalLines, kind, name, signature);
        };
        // ANTS-2148 follow-up — emit at an explicit line. A multi-line header
        // resolves to its START line (where the name sits), not the closing-
        // paren line, so read_region symbol-mode returns the full definition.
        auto offerAt = [&](int lineNo, const char *kind,
                           const QString &name, const QString &signature) {
            ++seenSymbols;
            if (symbols.size() >= maxSymbols) {
                truncated = true;
                return;
            }
            appendSymbol(symbols, lineNo, kind, name, signature);
        };

        // ANTS-3800 — GLSL rides the Cpp extractor rather than getting a lane
        // of its own: shader declarations ARE C declarations in shape, and a
        // parallel regex set would drift from this one silently.
        if (effective == Mode::Cpp || effective == Mode::Glsl) {
            // ANTS-2159 — scope-aware. A function/member symbol is emitted
            // only at file or type-body scope (funcOpenAtDepth < 0); inside
            // a code body the func paths are suppressed so a local
            // `Type name(arg);` or a `case X: return f();` statement is not
            // mistaken for a function. Match order: type-decl, qualified
            // member-def, free-func, multi-line func header, old-style
            // (return type on the prior line), Qt-marker, bare-type
            // (records a pending return type for the next line).
            const bool inFuncBody = (funcOpenAtDepth >= 0);
            const QString prevPendingType = pendingType;
            pendingType.clear();
            bool funcDefOpensBody = false;   // a definition whose body opens (now or on a later '{')
            QRegularExpressionMatch m;

            // ANTS-2148 follow-up — one literal/comment-aware pass yields both
            // the brace delta (scope tracking, below) and the paren delta (the
            // wrapped-parameter-list collector here), so inBlockComment toggles
            // exactly once per line.
            int parenDeltaThisLine = 0;
            int lastCodeIdx        = -1;
            const int braceDeltaThisLine =
                netBraceDelta(line, inBlockComment, &parenDeltaThisLine,
                              &lastCodeIdx);
            // ANTS-3735 — the declaration/definition discriminator. A trailing
            // comment must not hide the ';': `extern "C" int f(char* c);  // n`
            // is a DECLARATION, but `line.trimmed().endsWith(';')` reads it as a
            // definition whose body opens later. The scanner then adopted the
            // next `{` it met — an anonymous `namespace {` 12 lines down in
            // DOOM's r_vulkan.cpp — as that function's body and suppressed every
            // func symbol for the 5,742 lines until the namespace closed.
            const bool endsWithSemicolon =
                lastCodeIdx >= 0 && line.at(lastCodeIdx) == QLatin1Char(';');

            // ANTS-3738 — the same `lastCodeIdx`, reused as a comment-stripped
            // view for MATCHING. The three function anchors all end-anchor, so
            // a trailing comment sat in the way of every one of them:
            // rxCppFuncOpen anchors `\s*$`, rxCppFunc needs `[{;]` right after
            // the qualifier run, and rxCppFuncHeaderOpen needs `\([^)]*$` — so
            // `void CreateInstance()   // set up` matched NONE and the symbol
            // vanished. Same family as ANTS-3735 (raw-line matching against a
            // comment-aware brace scan), one site further on.
            //
            // Deliberately used for matching ONLY. `offer()` is still handed
            // the raw `line`, because `signature` carries the source text
            // including its trailing comment and changing that is an
            // output-shape decision, not a side effect of this fix. The
            // ANTS-3738 test asserts both halves.
            const QString codeLine =
                lastCodeIdx >= 0 ? line.left(lastCodeIdx + 1) : QString();

            if (inFuncArgs) {
                // Folding continuation lines of a wrapped parameter list into
                // the signature until the parens balance; then emit at the
                // header's start line.
                if (!funcArgSig.isEmpty()) funcArgSig += QLatin1Char(' ');
                funcArgSig += line.trimmed();
                funcArgParenDepth += parenDeltaThisLine;
                if (funcArgParenDepth <= 0) {
                    offerAt(funcArgStartLine, "func", funcArgName,
                            funcArgSig.simplified());
                    funcDefOpensBody = !endsWithSemicolon;
                    inFuncArgs = false;
                    funcArgSig.clear();
                }
            } else if ((m = rxCppTypedefStructOpen().match(line)).hasMatch()) {
                // ANTS-2228 — `typedef struct [TAG] {`. Record the opening;
                // the symbol(s) emit at the matching close line (where the
                // alias lives). `braceDepth` here is the pre-delta scope depth
                // this struct body sits above.
                pendingTypedefs.append({totalLines, m.captured(1), braceDepth});
            } else if ((m = rxCppType().match(line)).hasMatch()) {
                // ANTS-4101 — a namespace is not a class. Games Hub reported
                // a caller filtering on `kind` ("show me the classes in this
                // file") getting the enclosing namespace back as one of them.
                // rxCppType already captures the keyword, so the accurate kind
                // costs nothing. read_region's aggregate test keys off
                // kind=="class" AND the signature keyword, so a namespace
                // still resolves to its declaration only, exactly as before.
                offer(m.captured(1) == QLatin1String("namespace") ? "namespace"
                                                                  : "class",
                      m.captured(2), line);   // type/namespace body — never code
            } else if (!inFuncBody && (m = rxCppMember().match(line)).hasMatch()) {
                // ANTS-3399 — the qualified member name (`Class::method`,
                // possibly `NS::Class::method`) is the last whitespace-
                // delimited token before '('. The old heuristic stripped
                // the return type by locating the space before the FIRST
                // `::`, which landed INSIDE a namespace-qualified return
                // type (`JPH::BodyID Class::method`) — no space precedes
                // that `::`, so the return type stayed glued onto the name
                // and broke read_region symbol-mode lookups (Vestige
                // feedback, 2/2 hits). rxCppMember's return type may now be
                // multiple tokens (ANTS-3433, `unsigned int Class::method`),
                // but the qualified name is still the run after the LAST
                // whitespace, so the extraction is unchanged.
                const int lparen = line.indexOf(QLatin1Char('('));
                QString name = (lparen > 0) ? line.left(lparen).trimmed() : line;
                int lastWs = -1;
                for (int i = name.size() - 1; i >= 0; --i) {
                    if (name.at(i).isSpace()) { lastWs = i; break; }
                }
                if (lastWs >= 0) name = name.mid(lastWs + 1);
                // ANTS-3433 — a `T &Class::method` / `T *Class::method` form
                // glues the ref/ptr onto the name token; strip it so the
                // emitted symbol is the bare `Class::method` read_region wants.
                while (!name.isEmpty() && (name.at(0) == QLatin1Char('&') ||
                                          name.at(0) == QLatin1Char('*'))) {
                    name = name.mid(1);
                }
                offer("func", name, line);
                funcDefOpensBody = !endsWithSemicolon;
            } else if (!inFuncBody && (m = rxCppFunc().match(codeLine)).hasMatch()) {
                offer("func", m.captured(2), line);
                funcDefOpensBody = !endsWithSemicolon;
            } else if (!inFuncBody && (m = rxCppFuncOpen().match(codeLine)).hasMatch()) {
                offer("func", m.captured(2), line);    // ReturnType name(args), body '{' next line
                funcDefOpensBody = true;
            } else if (!inFuncBody && !prevPendingType.isEmpty()
                       && (m = rxCppNameArgs().match(line)).hasMatch()) {
                offer("func", m.captured(1), line);    // old-style: return type on the prior line
                funcDefOpensBody = true;
            } else if (!inFuncBody
                       && (m = rxCppFuncHeaderOpen().match(codeLine)).hasMatch()) {
                // ANTS-2148 follow-up — `ReturnType name(` opens a parameter
                // list that does not close on this line. Start collecting; the
                // closing line emits the symbol at funcArgStartLine.
                funcArgName       = m.captured(2);
                funcArgStartLine  = totalLines;
                funcArgSig        = line.trimmed();
                funcArgParenDepth = parenDeltaThisLine;   // >= 1 by construction
                inFuncArgs        = true;
            } else if ((m = rxCppQt().match(line)).hasMatch()) {
                offer("qt", m.captured(1), line);
            } else if (!inFuncBody && rxCppTypeOnly().match(line).hasMatch()) {
                pendingType = line;                    // candidate return type for the next line
            }

            // Brace-scope bookkeeping (runs every C++ line). A definition
            // that opens a body records funcOpenAtDepth; the interior stays
            // suppressed until the matching '}' returns the depth to it.
            const int before = braceDepth;
            braceDepth += braceDeltaThisLine;
            if (funcDefOpensBody && funcOpenAtDepth < 0) {
                if (braceDepth > before) {             // body opened on this line
                    funcOpenAtDepth = before;
                    funcBodyEntered = true;
                } else if (!line.contains(QLatin1Char('{'))) {  // await '{' on a later line
                    funcOpenAtDepth = before;
                    funcBodyEntered = false;
                }
                // else: single-line body ('{' opened and closed) → stay at file scope
            } else if (funcOpenAtDepth >= 0) {
                if (braceDepth > funcOpenAtDepth) funcBodyEntered = true;
                if (funcBodyEntered && braceDepth <= funcOpenAtDepth) {
                    funcOpenAtDepth = -1;
                    funcBodyEntered = false;
                }
            }

            // ANTS-2228 — close pending typedef-struct(s) whose brace balanced
            // back on this line. Emit at the recorded start line, keyed by the
            // alias (from `} ALIAS;`) and, when distinct, the tag. The
            // signature begins "struct" so resolveSymbol's aggregate-body path
            // (ANTS-2222) reads the FULL body via brace-match.
            while (!pendingTypedefs.isEmpty() &&
                   braceDepth <= pendingTypedefs.last().depthAtOpen) {
                const PendingTypedef pt = pendingTypedefs.takeLast();
                const QRegularExpressionMatch cm =
                    rxCppTypedefStructClose().match(line);
                const QString alias =
                    cm.hasMatch() ? cm.captured(1) : QString();
                const QString sig = pt.tag.isEmpty()
                    ? QStringLiteral("struct")
                    : (QStringLiteral("struct ") + pt.tag);
                if (!alias.isEmpty())
                    offerAt(pt.startLine, "class", alias, sig);
                if (!pt.tag.isEmpty() && pt.tag != alias)
                    offerAt(pt.startLine, "class", pt.tag, sig);
            }
        } else if (effective == Mode::Py) {
            QRegularExpressionMatch m = rxPy().match(line);
            if (m.hasMatch()) {
                const int indent      = m.captured(1).size();
                const QString keyword = m.captured(3);
                const QString name    = m.captured(4);
                // Dedent: drop any enclosing class we've left. A sibling/
                // outer def or class at ≤ the class's indent is no longer
                // inside it.
                while (!pyClassStack.isEmpty() &&
                       pyClassStack.last().first >= indent) {
                    pyClassStack.removeLast();
                }
                const QString prefix = pyClassStack.isEmpty()
                    ? QString()
                    : pyClassStack.last().second + QLatin1Char('.');
                const QString qualified = prefix + name;
                if (keyword == QLatin1String("class")) {
                    offer("class", qualified, line);
                    // A method (def at deeper indent) belongs to this class.
                    pyClassStack.append(qMakePair(indent, qualified));
                } else {
                    offer("func", qualified, line);
                }
            } else {
                // ANTS-4379 — a module-level constant. Only at column 0, and
                // only outside any class body still on the stack (a top-level
                // assignment after a class has ended is a constant; one
                // inside it is a class attribute).
                QRegularExpressionMatch cm = rxPyConst().match(line);
                if (cm.hasMatch()) {
                    pyClassStack.clear();   // column 0 ⟹ outside every class
                    // The signature stops at the `=`: a constant's value may
                    // be a 40-line dict that would otherwise ride along in
                    // every outline response (same rule as ANTS-4090's).
                    offer("const", cm.captured(1),
                          line.left(cm.capturedEnd(0)).trimmed() +
                              QStringLiteral(" …"));
                }
            }
        } else if (effective == Mode::Md) {
            // ANTS-4520 — a heading inside a blockquote is a heading. A plan
            // file whose run-state blocks are `> ## You are here` /
            // `> ## Previously` outlined to the plain headings only, so the
            // blocks a session is told to read FIRST were invisible in the one
            // call meant to answer "what is in this file?". The `>` does not
            // change the depth, so the ANTS-4396 filter treats it as a `##`.
            //
            // ANTS-4722 — fenced code is not document structure. Without this
            // guard a `# src/pkg/mod.py` label at column 0 inside a ```python
            // block is emitted as a level-1 heading, and the SIZING is the
            // damage rather than the phantom row: sizes:true scopes a
            // heading's extent to the next same-or-higher heading, so a
            // level-1 phantom in a document whose real sections are `##`
            // absorbs everything below it to EOF and every real section reads
            // as its child, with its own size wrong. max_heading_level is no
            // escape — the phantom is level 1, and any real filter keeps
            // level 1. Labelling a fenced source listing with a path comment
            // is an ordinary shape in a technical spec, not an edge case.
            //
            // Measured against the blockquote strip, like the heading match
            // below, so a fenced block quoted inside a `>` is tracked too.
            const QString mdLine = MarkdownScan::stripBlockquote(line);
            int openerRun = 0;
            const QChar opener =
                MarkdownScan::fenceOpenerChar(mdLine, 3, &openerRun);
            if (!mdFenceChar.isNull()) {
                // Inside a fence. CommonMark § 4.5 closes one on a line
                // opening with the same fence character in a run AT LEAST AS
                // LONG as the opener; either way nothing in here is a
                // heading, the closing line included.
                // ANTS-4820 — the length half was missing, and the comment
                // above used to state the rule without it, so a ``` line
                // ended a ```` block and its sample headings became real.
                if (MarkdownScan::fenceCloses(mdLine, mdFenceChar, mdFenceRun)) {
                    mdFenceChar = QChar();
                    mdFenceRun  = 0;
                }
                continue;
            }
            if (!opener.isNull()) {
                mdFenceChar = opener;
                mdFenceRun  = openerRun;
                continue;
            }
            QRegularExpressionMatch m = rxMdHeading().match(mdLine);
            if (m.hasMatch()) {
                const int level = m.captured(1).size();
                if (withSizes) mdLevelByLine.insert(totalLines, level);
                // ANTS-4396 — the depth filter. Applied at OFFER time rather
                // than as a post-pass, so a filtered outline is not silently
                // competing with maxSymbols for the same budget: dropping the
                // `###` noise must free room for the `##` headings further
                // down the file, which is the whole point on an append-only
                // log.
                if (maxHeadingLevel <= 0 || level <= maxHeadingLevel)
                    offer("heading", m.captured(2), line);
            }
        } else if (effective == Mode::Html) {
            // ANTS-4361 — LANDMARKS, deliberately not a DOM parse. Scoping to
            // landmarks is what keeps this out of the tar pit of parsing HTML
            // properly, and it is enough: the questions a caller has about a
            // single-file page are "where does the script start", "where is
            // the style block", "where is the element I am editing".
            //
            // The valuable half is the JS, and it is nearly free — the brace
            // family this outliner already parses well is exactly what is
            // inside a <script>, so the SAME regexes run over those lines.
            // Only the file's extension ever routed it to the fallback.
            const QString lower = line.toLower();
            if (lower.contains(QStringLiteral("<script"))) {
                inHtmlScript = !lower.contains(QStringLiteral("</script"));
                // A `type=` that is not JavaScript (application/json,
                // text/template) must NOT be handed to the brace parser —
                // its contents are data, and a JSON blob would emit noise.
                htmlScriptIsJs =
                    !lower.contains(QStringLiteral("type=\"application/json\"")) &&
                    !lower.contains(QStringLiteral("type=\"text/template\""));
                offer("region", QStringLiteral("<script>"), line.trimmed());
            } else if (lower.contains(QStringLiteral("</script"))) {
                inHtmlScript = false;
            } else if (lower.contains(QStringLiteral("<style"))) {
                offer("region", QStringLiteral("<style>"), line.trimmed());
            } else if (inHtmlScript && htmlScriptIsJs) {
                // Delegate to the brace-family set, which already emits
                // kind:"const" for a top-level `const NAME =` (ANTS-4090).
                QRegularExpressionMatch m = rxGenericDecl().match(line);
                if (m.hasMatch()) {
                    offer("func", m.captured(2), line);
                } else if ((m = rxGenericMethod().match(line)).hasMatch()) {
                    offer("func", m.captured(1), line);
                } else if ((m = rxGenericArrow().match(line)).hasMatch()) {
                    offer("func", m.captured(1), line);
                } else if ((m = rxGenericBinding().match(line)).hasMatch()) {
                    offer("const", m.captured(1),
                          line.left(m.capturedEnd(0)).trimmed() +
                              QStringLiteral(" …"));
                }
            } else {
                // Every element carrying an `id=` is an anchor a caller can
                // navigate to — outside a script, where an `id:` in JS would
                // be a property name rather than a landmark.
                QRegularExpressionMatch idm = rxHtmlId().match(line);
                if (idm.hasMatch())
                    offer("anchor", idm.captured(1), line.trimmed());
            }
        } else if (effective == Mode::Generic) {
            // Try keyword decl, then C-style method, then arrow assignment;
            // first hit wins (ANTS-2150).
            QRegularExpressionMatch m = rxGenericDecl().match(line);
            if (m.hasMatch()) {
                const QString kw = m.captured(1);
                const char *kind =
                    (kw == QLatin1String("fn")  || kw == QLatin1String("fun") ||
                     kw == QLatin1String("func")|| kw == QLatin1String("function") ||
                     kw == QLatin1String("def")) ? "func"
                    : (kw == QLatin1String("type") || kw == QLatin1String("module") ||
                       kw == QLatin1String("namespace")) ? "type"
                    : "class";
                offer(kind, m.captured(2), line);
            } else if ((m = rxGenericMethod().match(line)).hasMatch()) {
                offer("func", m.captured(1), line);
            } else if ((m = rxGenericArrow().match(line)).hasMatch()) {
                offer("func", m.captured(1), line);
            } else if ((m = rxGenericBinding().match(line)).hasMatch()) {
                // ANTS-4090 — its own kind, so a caller can tell a data
                // binding from a function. The signature stops at the `=`:
                // the value may be a 95-line template literal whose first
                // line would otherwise ride along in every outline response.
                offer("const", m.captured(1),
                      line.left(m.capturedEnd(0)).trimmed() +
                          QStringLiteral(" …"));
            }
        }
    }
    f.close();

    // ANTS-2150 — report the precise brace-family language (rust/go/typescript…)
    // rather than the shared "generic" mode name.
    QString languageStr = QString::fromLatin1(modeToLanguageString(effective));
    if (effective == Mode::Generic) {
        const QString precise = genericLangName(QFileInfo(absPath).suffix().toLower());
        if (!precise.isEmpty()) languageStr = precise;
    }

    QJsonObject out;
    out["ok"]          = true;
    out["path"]        = absPath;
    out["language"]    = languageStr;
    out["header_doc"]  = headerDoc;
    out["symbols"]     = symbols;
    out["truncated"]   = truncated;
    // ANTS-4472's sibling, ANTS-4469 — a bare `truncated:true` cannot tell a
    // caller whether it missed one symbol or four hundred, which is exactly
    // the fact that decides whether to re-call with a raised cap or accept the
    // outline as effectively complete. The asymmetry was inside this one verb:
    // the max_bytes path already emits `symbols_dropped` (ANTS-1293) and
    // `filter` already emits symbols_considered / symbols_filtered_out
    // (ANTS-4374), leaving max_symbols as the only narrowing mechanism of the
    // three reporting no number — and the only one with a non-zero DEFAULT
    // (200), so also the one most likely to fire unasked. `seenSymbols` is
    // incremented on every regex match whether or not the symbol was kept, so
    // the count is exact rather than an estimate; the field name is reused
    // from the max_bytes path deliberately, so one caller branch handles both.
    if (truncated)
        out["symbols_dropped"] = seenSymbols - symbols.size();
    out["total_bytes"] = static_cast<double>(totalBytes);
    out["total_lines"] = totalLines;
    // ANTS-4366 — "this file genuinely has no top-level symbols" and "the
    // outliner could not read this file" were byte-identical replies, which is
    // what turned a parser gap into a silently wrong answer: a caller
    // reasonably concluded a 1935-line file had no functions. Emit a truthy
    // marker on the second case only. Truthy on purpose — `compact:true` drops
    // an empty `symbols` array entirely, so the ABSENCE of symbols cannot
    // carry the signal, and a `false` would be dropped too.
    //
    // Deliberately narrow: a recognised language AND a non-trivial file. An
    // unknown extension already reports `language:"unknown"`, and a 3-line
    // header stub having no symbols is not evidence of anything.
    // ANTS-4384 — per-symbol extents. Computed in DOCUMENT order, not array
    // order: a symbol can be appended out of order (a typedef-struct emits at
    // its START line only once its brace balances, by which point later
    // symbols are already in the array), and array order would then hand it a
    // negative or whole-file span.
    if (withSizes && !symbols.isEmpty()) {
        lineStart.append(totalBytes);   // sentinel: start of the line past EOF

        QVector<QPair<int, int>> order;   // (start line, index into symbols)
        order.reserve(symbols.size());
        for (int i = 0; i < symbols.size(); ++i)
            order.append({symbols.at(i).toObject().value("line").toInt(), i});
        std::stable_sort(order.begin(), order.end(),
                         [](const auto &a, const auto &b) {
                             return a.first < b.first;
                         });

        const auto levelAt = [&](int line) {
            return mdLevelByLine.value(line, 1);
        };
        for (int k = 0; k < order.size(); ++k) {
            const int startLine = order.at(k).first;
            if (startLine < 1 || startLine > totalLines) continue;
            const int level = levelAt(startLine);
            int endLine = totalLines;              // inclusive; EOF by default
            for (int j = k + 1; j < order.size(); ++j) {
                const int nextLine = order.at(j).first;
                if (nextLine <= startLine) continue;   // same-line siblings
                if (levelAt(nextLine) <= level) { endLine = nextLine - 1; break; }
            }
            // ANTS-4374's invariant — a number is only emitted where the walk
            // actually saw the end of the range. `truncated` means symbols
            // were dropped from the tail, so the last retained symbol's extent
            // would silently absorb everything the cap hid.
            if (truncated && k == order.size() - 1) continue;
            if (endLine < startLine) continue;
            QJsonObject s = symbols.at(order.at(k).second).toObject();
            s["lines"] = endLine - startLine + 1;
            s["bytes"] = static_cast<double>(lineStart.at(endLine) -
                                             lineStart.at(startLine - 1));
            symbols[order.at(k).second] = s;
        }
        out["symbols"] = symbols;
    }

    if (symbols.isEmpty() && totalLines > 10
        && languageStr != QLatin1String("unknown")
        && !languageStr.isEmpty()) {
        out["parse_empty"] = true;
        out["hint"] = QStringLiteral(
            "no symbol matched in a recognised %1 file of %2 lines — this is "
            "the outliner finding nothing, which reads the same as a file that "
            "has nothing. If the file plainly declares symbols, it is a parser "
            "gap worth reporting.").arg(languageStr).arg(totalLines);
    }
    return out;
}

}  // namespace FileOutline
