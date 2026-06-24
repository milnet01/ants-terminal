// ANTS-1249 — file_outline regex scanner. See fileoutline.h header.

#include "fileoutline.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QString>

namespace FileOutline {

namespace {

constexpr int kMaxLineBytes      = 1024;   // ANTS-1249-INV-8
constexpr int kMaxSymbolsCap     = 1000;
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
        QRegularExpression r(QStringLiteral(R"(^[\w:]+\s+[\w:]+::[\w~]+\s*\()"));
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
    // that the return-type group `(?:[\w:<>]+[\s*&]+)++` would otherwise
    // absorb, emitting the call site as a spurious function symbol. The
    // negative lookahead rejects an expression-introducing reserved
    // keyword as the first token — a keyword is never a return type, so
    // no real definition is lost. Mirrors the symbolquery.cpp guard.
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(static|inline|template[^>]*>)?\s*(?!(?:return|co_return|co_await|co_yield|throw|else)\b)(?:[\w:<>]+[\s*&]+)++(\w+)\s*\([^)]*\)\s*[{;])"));
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
const QRegularExpression &rxPy() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^(async\s+)?(def|class)\s+(\w+))"));
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

// (1) keyword declarations: optional modifiers, a declaration keyword, an
// optional Go receiver `(s *T)`, then the name. Captures kw=group1, name=group2.
const QRegularExpression &rxGenericDecl() {
    static const QRegularExpression rx = []{
        QRegularExpression r(QStringLiteral(R"(^\s*(?:(?:pub|export|default|public|private|protected|internal|static|final|abstract|sealed|async|open|override|suspend|inline|const|unsafe|extern|data)\s++)*+(fn|fun|func|function|def|class|struct|enum|trait|impl|interface|type|module|object|protocol|extension|namespace|record)\b(?:\s++\([^)]*\))?\s++([A-Za-z_$][\w$]*))"));
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
        case Mode::Auto:    return "unknown";
    }
    return "unknown";
}

QString headerCommentMarker(Mode m) {
    switch (m) {
        case Mode::Cpp:     return QStringLiteral("//");
        case Mode::Generic: return QStringLiteral("//");  // brace family (ANTS-2150)
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
    if (s == QLatin1String("json")) return Mode::Json;
    if (s == QLatin1String("generic")) return Mode::Generic;
    return Mode::Auto;
}

QJsonObject compute(const QString &absPath,
                    Mode mode,
                    bool includeDocComment,
                    int maxSymbols) {
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
    int seenSymbols = 0;
    bool truncated = false;
    bool inHeaderBlock = includeDocComment && !isUnknown &&
                         effective != Mode::Json;
    const QString headerMarker = headerCommentMarker(effective);
    int headerLinesEmitted = 0;

    while (!f.atEnd()) {
        const QByteArray rawLine = f.readLine();
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

        if (effective == Mode::Cpp) {
            // Match order: type-decl, member-def (qualified),
            // free-func, Qt-marker. Short-circuit on the first hit.
            QRegularExpressionMatch m;
            m = rxCppType().match(line);
            if (m.hasMatch()) {
                const QString name = m.captured(2);
                offer("class", name, line);
                continue;
            }
            m = rxCppMember().match(line);
            if (m.hasMatch()) {
                // Name = up to the first '(' in the line.
                const int lparen = line.indexOf(QLatin1Char('('));
                QString name = (lparen > 0) ? line.left(lparen).trimmed()
                                            : line;
                // Strip leading "ReturnType " — keep "Class::method".
                const int firstColon = name.indexOf(QStringLiteral("::"));
                if (firstColon > 0) {
                    const int spaceBeforeName = name.lastIndexOf(
                        QLatin1Char(' '), firstColon);
                    if (spaceBeforeName > 0) {
                        name = name.mid(spaceBeforeName + 1);
                    }
                }
                offer("func", name, line);
                continue;
            }
            m = rxCppFunc().match(line);
            if (m.hasMatch()) {
                offer("func", m.captured(2), line);
                continue;
            }
            m = rxCppQt().match(line);
            if (m.hasMatch()) {
                offer("qt", m.captured(1), line);
                continue;
            }
        } else if (effective == Mode::Py) {
            QRegularExpressionMatch m = rxPy().match(line);
            if (m.hasMatch()) {
                const QString keyword = m.captured(2);
                const QString name = m.captured(3);
                offer(keyword == QLatin1String("class") ? "class" : "func",
                      name, line);
            }
        } else if (effective == Mode::Md) {
            QRegularExpressionMatch m = rxMdHeading().match(line);
            if (m.hasMatch()) {
                offer("heading", m.captured(2), line);
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
    out["total_bytes"] = static_cast<double>(totalBytes);
    out["total_lines"] = totalLines;
    return out;
}

}  // namespace FileOutline
