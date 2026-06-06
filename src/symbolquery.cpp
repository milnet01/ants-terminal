// ANTS-1303 — find_definition / find_caller regex scanner.
// See symbolquery.h header.

#include "symbolquery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

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
        case Lang::Cpp:  return "cpp";
        case Lang::Py:   return "py";
        case Lang::Lua:  return "lua";
        case Lang::Sh:   return "sh";
        case Lang::Auto: return "";
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
    return Lang::Auto;
}

// Per-call compiled anchors for one language. `def` holds 1-2 patterns
// (a line is a definition if any matches); `call` is the call anchor.
// `cppKind` selects the trailing-`;` declaration/definition split.
struct Anchors {
    QVector<QRegularExpression> def;
    QRegularExpression call;
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
            add(QStringLiteral("^[ \\t]*(?:[\\w:<>~]+[\\s*&]+)+(?:[\\w:]+::)?") + s + QStringLiteral("\\s*\\("));
            // Out-of-line constructor / destructor definitions carry no
            // return type (`Foo::Foo(` / `Foo::~Foo(`); match them
            // explicitly so a class query still resolves its ctor/dtor.
            // (In-class ctor *declarations* — `Foo(` with no qualifier —
            // are indistinguishable from a call and intentionally left out.)
            add(QStringLiteral("^[ \\t]*") + s + QStringLiteral("::~?") + s + QStringLiteral("\\s*\\("));
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
    Anchors anchors[5];

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
};

void scanFile(ScanState &st, const QFileInfo &fi, const Anchors &an) {
    QFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;  // unopenable → not counted
    ++st.filesScanned;

    const QString rel = fi.absoluteFilePath().mid(st.rootPrefixLen);
    int lineNo = 0;
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
        if (raw.size() > kMaxLineBytes) continue;  // backtracking guard
        const QString line = QString::fromUtf8(raw);

        bool isDef = false;
        for (const QRegularExpression &re : an.def) {
            if (re.match(line).hasMatch()) { isDef = true; break; }
        }

        if (isDef && st.collectDefs) {
            ++st.defsTotal;
            const QString trimmed = line.trimmed();
            QString kind = QStringLiteral("definition");
            if (an.cppKind && trimmed.endsWith(QLatin1Char(';')))
                kind = QStringLiteral("declaration");
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
        if (st.fileStemHit.isEmpty() && !st.symbol.isEmpty()
            && fi.completeBaseName() == st.symbol)
            st.fileStemHit = fi.absoluteFilePath().mid(st.rootPrefixLen);

        if (st.langFilter != Lang::Auto && lang != st.langFilter) continue;

        if (st.filesScanned >= st.maxFiles) { st.walkCapped = true; return; }
        scanFile(st, fi, st.anchors[static_cast<int>(lang)]);
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

    // definitions first, then declarations; each already in walk
    // (path) order. Truncate the combined list to the cap.
    r.definitions = st.defsDefn;
    for (const DefMatch &d : st.defsDecl) r.definitions.append(d);
    if (r.definitions.size() > st.defCap) r.definitions.resize(st.defCap);

    r.definitionsTotal = st.defsTotal;
    r.filesScanned = st.filesScanned;
    r.walkCapped = st.walkCapped;
    r.truncated = st.defsTotal > r.definitions.size();
    // ANTS-1950 — only a hint when there was nothing to find: a real symbol
    // that also happens to share a file's stem should not be second-guessed.
    if (r.definitions.isEmpty())
        r.fileStemHint = st.fileStemHit;
    return r;
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

    walk(st, QDir::cleanPath(rootCanonical));

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
