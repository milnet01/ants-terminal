// ANTS-3661 — see docsymbols.h for the design constraint (report-only) and
// docs/specs/ANTS-3661.md for the contract.

#include "docsymbols.h"

#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <utility>

#include "markdownscan.h"

namespace DocSymbols {
namespace {

// candidate := ident ( "::" ident )* "()"?   (spec § 2.1, whole-span)
//
// The "no `/` and no `.`" exclusion is this production rather than a separate
// filter: a path cannot match it. Paths belong to doc_citations (ANTS-3636) and
// doc_integrity (ANTS-3601), and one `src/foo.cpp` must not be reported by two
// verbs with two vocabularies.
const QRegularExpression &candidateRe() {
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*(?:\\(\\))?$"));
    return re;
}

// A doc backticks these while explaining code; none is a symbol anyone
// declared. Union across the families SymbolQuery resolves, which is why `nil`,
// `elif` and `fi` sit beside `constexpr`.
const QSet<QString> &keywords() {
    static const QSet<QString> k = {
        QStringLiteral("if"),       QStringLiteral("else"),     QStringLiteral("elif"),
        QStringLiteral("for"),      QStringLiteral("while"),    QStringLiteral("do"),
        QStringLiteral("done"),     QStringLiteral("then"),     QStringLiteral("fi"),
        QStringLiteral("case"),     QStringLiteral("esac"),     QStringLiteral("switch"),
        QStringLiteral("return"),   QStringLiteral("break"),    QStringLiteral("continue"),
        QStringLiteral("class"),    QStringLiteral("struct"),   QStringLiteral("union"),
        QStringLiteral("enum"),     QStringLiteral("namespace"),QStringLiteral("template"),
        QStringLiteral("typename"), QStringLiteral("typedef"),  QStringLiteral("using"),
        QStringLiteral("public"),   QStringLiteral("private"),  QStringLiteral("protected"),
        QStringLiteral("virtual"),  QStringLiteral("override"), QStringLiteral("final"),
        QStringLiteral("static"),   QStringLiteral("const"),    QStringLiteral("constexpr"),
        QStringLiteral("inline"),   QStringLiteral("explicit"), QStringLiteral("friend"),
        QStringLiteral("operator"), QStringLiteral("new"),      QStringLiteral("delete"),
        QStringLiteral("this"),     QStringLiteral("self"),     QStringLiteral("nullptr"),
        QStringLiteral("null"),     QStringLiteral("nil"),      QStringLiteral("none"),
        QStringLiteral("true"),     QStringLiteral("false"),    QStringLiteral("bool"),
        QStringLiteral("int"),      QStringLiteral("float"),    QStringLiteral("double"),
        QStringLiteral("char"),     QStringLiteral("void"),     QStringLiteral("auto"),
        QStringLiteral("def"),      QStringLiteral("lambda"),   QStringLiteral("import"),
        QStringLiteral("from"),     QStringLiteral("global"),   QStringLiteral("local"),
        QStringLiteral("function"), QStringLiteral("end"),      QStringLiteral("repeat"),
        QStringLiteral("until"),    QStringLiteral("try"),      QStringLiteral("catch"),
        QStringLiteral("throw"),    QStringLiteral("raise"),    QStringLiteral("except"),
        QStringLiteral("finally"),  QStringLiteral("yield"),    QStringLiteral("async"),
        QStringLiteral("await"),    QStringLiteral("pass"),     QStringLiteral("and"),
        QStringLiteral("or"),       QStringLiteral("not"),      QStringLiteral("in"),
        QStringLiteral("is"),       QStringLiteral("with"),     QStringLiteral("as"),
    };
    return k;
}

// CommonMark § 6.1: one leading AND one trailing space are stripped from a code
// span's content when both are present and the content is not all spaces.
QString stripOneSpace(const QString &content) {
    if (content.size() >= 2 && content.startsWith(QLatin1Char(' '))
        && content.endsWith(QLatin1Char(' ')) && !content.trimmed().isEmpty())
        return content.mid(1, content.size() - 2);
    return content;
}

struct Candidate {
    QString span;    // verbatim span content, echoed as Symbol::symbol
    QString needle;  // what the resolver is asked for
    int     line = 0;    // 1-based
    int     col = 0;     // 0-based
    bool    ambiguous = false;  // bare lowercase word — see the emission rule (ANTS-3692)
};

}  // namespace

QString resolutionStr(Resolution r) {
    switch (r) {
    case Resolution::Resolved:   return QStringLiteral("resolved");
    case Resolution::Unresolved: return QStringLiteral("unresolved");
    case Resolution::NotChecked: return QStringLiteral("not_checked");
    }
    return QString();
}

ScanResult scan(const QString &text, const QString &relPath, const Options &opts) {
    ScanResult res;

    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);

    const QVector<bool> fence   = MarkdownScan::fenceMask(lines);
    const QVector<bool> example = MarkdownScan::exampleMask(lines, fence);
    const QVector<MarkdownScan::CodeSpan> spans = MarkdownScan::codeSpans(lines, fence);

    QVector<Candidate> cands;
    for (const MarkdownScan::CodeSpan &s : spans) {
        // A span crossing a newline cannot match the production whole-span
        // (CommonMark folds the line ending to a space, and a space fails it),
        // so skipping one is equivalent to joining and re-testing it.
        if (s.startLine != s.endLine) continue;
        // A doc teaching symbol resolution names fictional symbols on purpose,
        // exactly as it writes fictional paths (ANTS-3659). Honouring the
        // region is this verb's own decision, not an inheritance.
        if (example.value(s.startLine)) continue;

        const QString span =
            stripOneSpace(lines.at(s.startLine).mid(s.startCol, s.endCol - s.startCol));
        if (!candidateRe().match(span).hasMatch()) continue;
        if (opts.excludedNames.contains(span)) continue;
        if (keywords().contains(span)) continue;

        // The length floor is a real trade: it also drops `emit`, `exec`,
        // `main` and `open`. Accepted, because a doc asserting something about
        // one of those almost always writes it with `()` or a scope, and the
        // alternative is a list dominated by prose emphasis.
        const bool unambiguousShape = span.contains(QLatin1String("::"))
                                      || span.endsWith(QLatin1String("()"))
                                      || span != span.toLower();
        if (!unambiguousShape && span.size() < opts.minIdentChars) continue;

        // Two reductions, IN THIS ORDER: `Foo::bar()` must become `bar`, never
        // `bar()`. Unstripped, the corpus's commonest candidate shape fails
        // SymbolQuery::isValidSymbol and turns into a verb refusal.
        QString needle = span;
        if (needle.endsWith(QLatin1String("()"))) needle.chop(2);
        const int scope = needle.lastIndexOf(QLatin1String("::"));
        if (scope >= 0) needle = needle.mid(scope + 2);
        if (!SymbolQuery::isValidSymbol(needle)) continue;  // >128 chars, etc.

        cands.push_back({span, needle, s.startLine + 1, s.startCol, !unambiguousShape});
    }

    std::stable_sort(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });

    // Distinct needles in first-appearance order — the set that costs a walk —
    // but UNAMBIGUOUS ones first (ANTS-3692). Bare lowercase spans outnumber
    // `()`/`::`/mixed-case ones about 17:1 across docs/*.md, and most are
    // dropped unreported by the emission rule below, so in document order the
    // budget is spent on names nobody will be told about while the symbols the
    // check exists for come back not_checked. A needle reachable by both shapes
    // is claimed by the first pass, which is the stronger of its two claims.
    QStringList order;
    QSet<QString> seen;
    for (const bool ambiguousPass : {false, true})
        for (const Candidate &c : cands)
            if (c.ambiguous == ambiguousPass && !seen.contains(c.needle)) {
                seen.insert(c.needle);
                order << c.needle;
            }

    QHash<QString, Resolution> state;
    QHash<QString, QVector<SymbolQuery::DefMatch>> defs;
    // Tracked over NEEDLES, not over emitted occurrences: an elided ambiguous
    // needle never reaches symbols[] (ANTS-3692), so deriving `truncated` from
    // notChecked > 0 would report a complete run over an incomplete one.
    bool anyNeedleElided = false;
    QElapsedTimer clock;
    clock.start();
    // ANTS-3680 — resolve a CHUNK of needles per tree walk instead of one, the
    // walk being the same walk every time. Measured on this tree: 200 needles
    // went 15.6 s to 0.35 s, same matches. The chunk is not a RAM bound (the
    // scan holds one file's lines whatever the needle count) — it is the only
    // point at which the deadline can still fire and report the rest
    // NotChecked honestly, which a single whole-document walk could not do.
    constexpr int kResolveChunk = 128;
    QStringList pending;
    pending.reserve(kResolveChunk);

    auto flush = [&]() {
        if (pending.isEmpty()) return;
        SymbolQuery::Options so;
        so.maxResults = 50;
        const QHash<QString, SymbolQuery::DefResult> batch =
            SymbolQuery::findDefinitions(opts.rootCanonical, pending, so);
        for (const QString &needle : std::as_const(pending)) {
            const SymbolQuery::DefResult r = batch.value(needle);
            ++res.needlesResolved;
            if (r.ok && !r.definitions.isEmpty()) {
                state[needle] = Resolution::Resolved;
                defs[needle]  = r.definitions;
            } else {
                state[needle] = Resolution::Unresolved;
            }
        }
        pending.clear();
    };

    for (const QString &needle : std::as_const(order)) {
        const bool outOfBudget =
            res.needlesResolved + pending.size() >= opts.maxSymbolsPerRun
            || opts.rootCanonical.isEmpty()
            || (opts.resolveDeadlineMs > 0 && clock.elapsed() >= opts.resolveDeadlineMs);
        if (outOfBudget) {
            state[needle] = Resolution::NotChecked;
            anyNeedleElided = true;
            continue;
        }
        pending << needle;
        if (pending.size() >= kResolveChunk) flush();
    }
    flush();

    for (const Candidate &c : std::as_const(cands)) {
        const Resolution r = state.value(c.needle, Resolution::NotChecked);

        // ANTS-3692 — an ambiguous span earns its place in the response only by
        // resolving. Sampled across docs/*.md, 60 of 100 bare lowercase spans
        // failed to resolve and every one was a JSON response key, a config key
        // or a refusal code (`counts`, `dry_run`, `truncated`); 7 resolved and
        // were real (`cmd_promote`, `redispatch`). Nothing here can tell an
        // unresolved key from genuine rot, so the engine stays silent instead of
        // guessing — § 2.3's report-never-judge rule, one level earlier.
        //
        // Resolution is the discriminator, deliberately NOT shape: requiring a
        // `::`/`()`/mixed-case span would drop shell, Python and Lua symbols
        // wholesale, since bare lowercase is those languages' naming convention.
        if (c.ambiguous && r != Resolution::Resolved) continue;

        Symbol sym;
        sym.symbol     = c.span;
        sym.docLine    = c.line;
        sym.docCol     = c.col;
        sym.resolution = r;
        if (sym.resolution == Resolution::Resolved) sym.definitions = defs.value(c.needle);
        res.symbols.push_back(sym);

        switch (sym.resolution) {
        case Resolution::Resolved:   ++res.resolved;   break;
        case Resolution::NotChecked: ++res.notChecked; break;
        case Resolution::Unresolved: {
            ++res.unresolved;
            DocFinding::Finding f;
            f.verb = QStringLiteral("doc_symbols");
            f.kind = QStringLiteral("unresolved_symbol");
            f.file = relPath;
            f.line = c.line;
            f.message = c.span == c.needle
                            ? QStringLiteral("no definition found for `%1`").arg(c.needle)
                            : QStringLiteral("no definition found for `%1` (from `%2`)")
                                  .arg(c.needle, c.span);
            // ANTS-4359 — the SHAPE of the span the document wrote, so the
            // flat unresolved bucket can be read by class instead of name by
            // name. On one standard the bucket was 24 of 58 checked: enum
            // values, a Qt macro, a CMake variable, a JSON key — every one
            // benign-looking, so the whole bucket was logged into a cold-review
            // brief as settled noise with three reviewers told not to
            // re-confirm it. ONE name in it was a genuine defect: the document
            // cited `isControlPlaneTool()` as "the canonical bypass list" and
            // no such symbol exists. It survived two full loops inside a
            // dismissed bucket.
            //
            // The discriminator is call-shape, because that is what separates
            // the two populations: a span written with `()` is a claim about a
            // FUNCTION, and "no such function" is a real defect; enum values,
            // macros, CMake variables and JSON keys are bare identifiers.
            // `isControlPlaneTool()` carried its parens, so it would have
            // sorted to the top of its own bucket rather than into the middle
            // of two dozen benign names.
            //
            // A classification, never a verdict — § 2.3's report-never-judge
            // rule. Nothing is filtered out and no bucket is dropped; the
            // reader still decides.
            const QString shape =
                c.span.contains(QStringLiteral("()"))
                    ? QStringLiteral("call")
                    : (c.span.contains(QStringLiteral("::"))
                           ? QStringLiteral("qualified")
                           : QStringLiteral("bare"));
            f.extra[QStringLiteral("shape")] = shape;
            if (shape == QLatin1String("call"))           ++res.unresolvedCall;
            else if (shape == QLatin1String("qualified")) ++res.unresolvedQualified;
            else                                          ++res.unresolvedBare;
            // autoFixable stays false: a reader decides whether this is rot or
            // a forward reference, so there is nothing here to repair.
            f.emissionIndex = res.findings.size();
            res.findings.push_back(f);
            break;
        }
        }
    }

    res.total     = res.symbols.size();
    res.truncated = anyNeedleElided;
    return res;
}

namespace {

// ANTS-5313 INV-9 — the class, struct and namespace names enclosing `line`
// (1-based) in a C-family file, outermost first. Strings and comments are
// skipped so a brace inside one opens nothing; a brace whose head carries no
// class-key or `namespace` opens an unnamed scope (a function body, an
// initialiser), which still has to be popped.
QStringList enclosingScopes(const QStringList &lines, int line) {
    static const QRegularExpression named(QStringLiteral(
        "\\b(?:class|struct|union|namespace)\\s+(?:[A-Z_][A-Z0-9_]*\\s+)?"
        "([A-Za-z_]\\w*)[^;{()]*$"));
    QStringList stack;
    QString head;
    bool inComment = false;
    for (int li = 0; li < line - 1 && li < lines.size(); ++li) {
        const QString &l = lines.at(li);
        for (qsizetype i = 0; i < l.size(); ++i) {
            const QChar c = l.at(i);
            if (inComment) {
                if (c == QLatin1Char('*') && i + 1 < l.size() && l.at(i + 1) == QLatin1Char('/')) {
                    inComment = false;
                    ++i;
                }
                continue;
            }
            if (c == QLatin1Char('/') && i + 1 < l.size()) {
                if (l.at(i + 1) == QLatin1Char('/')) break;
                if (l.at(i + 1) == QLatin1Char('*')) { inComment = true; ++i; continue; }
            }
            if (c == QLatin1Char('"')) {
                qsizetype j = i + 1;
                while (j < l.size() && l.at(j) != c) j += (l.at(j) == QLatin1Char('\\')) ? 2 : 1;
                i = j;
                continue;
            }
            if (c == QLatin1Char('{')) {
                const auto m = named.match(head.trimmed());
                stack << (m.hasMatch() ? m.captured(1) : QString());
                head.clear();
            } else if (c == QLatin1Char('}')) {
                if (!stack.isEmpty()) stack.removeLast();
                head.clear();
            } else if (c == QLatin1Char(';')) {
                head.clear();
            } else {
                head += c;
            }
        }
        head += QLatin1Char(' ');
    }
    stack.removeAll(QString());
    return stack;
}

// Python: the `class` lines above `line` that each indent less than the last.
QStringList enclosingPyClasses(const QStringList &lines, int line) {
    static const QRegularExpression cls(QStringLiteral("^(\\s*)class\\s+([A-Za-z_]\\w*)"));
    if (line < 1 || line > lines.size()) return {};
    const QString &at = lines.at(line - 1);
    qsizetype indent = at.size() - at.trimmed().size();
    QStringList out;
    for (int li = line - 2; li >= 0 && indent > 0; --li) {
        const auto m = cls.match(lines.at(li));
        if (m.hasMatch() && m.captured(1).size() < indent) {
            out.prepend(m.captured(2));
            indent = m.captured(1).size();
        }
    }
    return out;
}

// Does candidate `d` live inside `qual`? Its own signature decides when it
// is written qualified (`void Alpha::fire(`); otherwise the scopes enclosing
// its line do. No root means only the signature can confirm.
bool inQualifier(const SymbolQuery::DefMatch &d, const QString &qual, const QString &leaf,
                 const SourceLines &sourceLines, QHash<QString, QStringList> &fileCache) {
    const QRegularExpression written(
        QStringLiteral("([A-Za-z_]\\w*)\\s*(?:::|\\.)\\s*~?") + QRegularExpression::escape(leaf)
        + QStringLiteral("\\b"));
    const auto m = written.match(d.signature);
    if (m.hasMatch()) return m.captured(1) == qual;
    if (!sourceLines) return false;
    auto it = fileCache.find(d.file);
    if (it == fileCache.end()) it = fileCache.insert(d.file, sourceLines(d.file));
    const QStringList chain = d.file.endsWith(QLatin1String(".py"))
                                  ? enclosingPyClasses(*it, d.line)
                                  : enclosingScopes(*it, d.line);
    return chain.contains(qual);
}

}  // namespace

// ANTS-5313 — see docsymbols.h. Occurrences of one span share a resolution
// (scan() caches per needle), so the first occurrence decides — except that a
// resolved occurrence outranks an unresolved or unchecked one, should a future
// scan ever split them.
Locators locate(const QVector<Symbol> &symbols, const SourceLines &sourceLines) {
    QHash<QString, QStringList> fileCache;
    QHash<QString, const Symbol *> first;
    QStringList order;
    for (const Symbol &s : symbols) {
        auto it = first.find(s.symbol);
        if (it == first.end()) {
            first.insert(s.symbol, &s);
            order << s.symbol;
        } else if (s.resolution == Resolution::Resolved
                   && (*it)->resolution != Resolution::Resolved) {
            *it = &s;
        }
    }

    Locators out;
    for (const QString &name : std::as_const(order)) {
        const Symbol &s = *first.value(name);
        if (s.resolution == Resolution::NotChecked) { out.notChecked << name; continue; }
        if (s.resolution == Resolution::Unresolved) { out.unresolved << name; continue; }

        // Distinct file:line per kind — a repeated match is one candidate.
        // A `local` row and a forward declaration are real declarations but
        // no place to send a reader (spec INV-8), so they are not candidates.
        static const QRegularExpression forwardDecl(QStringLiteral(
            "^(?:template\\s*<[^>]*>\\s*)?(?:class|struct|union|enum(?:\\s+class|\\s+struct)?)"
            "\\s+(?:[A-Z_][A-Z0-9_]*\\s+)?[A-Za-z_]\\w*\\s*;"));
        // INV-9 — `A::b` keeps only the candidates that live inside `A`;
        // with several qualifiers the innermost is checked. The default mode
        // resolves on the leaf (ANTS-3661 INV-2), which suits a candidate
        // list; a single answer must not cross into another class.
        QString bare = name;
        if (bare.endsWith(QLatin1String("()"))) bare.chop(2);
        const qsizetype sep = bare.lastIndexOf(QLatin1String("::"));
        QString qual;
        if (sep > 0) {
            qual = bare.left(sep);
            qual = qual.mid(qual.lastIndexOf(QLatin1String("::")) + 1).remove(QLatin1Char(':'));
        }
        const QString leaf = sep > 0 ? bare.mid(sep + 2) : bare;

        QStringList defs, decls;
        bool declaredElsewhere = false;
        for (const SymbolQuery::DefMatch &d : s.definitions) {
            if (!qual.isEmpty() && !inQualifier(d, qual, leaf, sourceLines, fileCache))
                continue;
            if (d.kind == QLatin1String("local")
                || forwardDecl.match(d.signature).hasMatch()) {
                declaredElsewhere = true;
                continue;
            }
            const QString loc = d.file + QLatin1Char(':') + QString::number(d.line);
            QStringList &bucket =
                d.kind == QLatin1String("definition") ? defs : decls;
            if (!bucket.contains(loc)) bucket << loc;
        }
        const QStringList &pick = defs.isEmpty() ? decls : defs;
        if (pick.size() == 1)      out.located.insert(name, pick.first());
        else if (pick.size() > 1) out.ambiguous.insert(name, int(pick.size()));
        else if (declaredElsewhere) out.declaredOnly << name;
        else                       out.unresolved << name;  // resolved with no match
    }
    return out;
}

}  // namespace DocSymbols
