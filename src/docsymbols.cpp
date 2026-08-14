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
    int     line;    // 1-based
    int     col;     // 0-based
    bool    ambiguous;  // bare lowercase word — see the emission rule (ANTS-3692)
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
    for (const QString &needle : std::as_const(order)) {
        const bool outOfBudget =
            res.needlesResolved >= opts.maxSymbolsPerRun
            || opts.rootCanonical.isEmpty()
            || (opts.resolveDeadlineMs > 0 && clock.elapsed() >= opts.resolveDeadlineMs);
        if (outOfBudget) {
            state[needle] = Resolution::NotChecked;
            anyNeedleElided = true;
            continue;
        }

        SymbolQuery::Options so;
        so.maxResults = 50;
        const SymbolQuery::DefResult r =
            SymbolQuery::findDefinition(opts.rootCanonical, needle, so);
        ++res.needlesResolved;
        if (r.ok && !r.definitions.isEmpty()) {
            state[needle] = Resolution::Resolved;
            defs[needle]  = r.definitions;
        } else {
            state[needle] = Resolution::Unresolved;
        }
    }

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
                    : (c.needle.contains(QStringLiteral("::"))
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

}  // namespace DocSymbols
