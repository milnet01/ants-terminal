// ANTS-5506 — doc_facts. See docfacts.h and docs/specs/ANTS-3663-doc-lint-every-deterministic-doc.md § 2.6.

#include "docfacts.h"
#include "markdownscan.h"
#include "specparse.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace DocFacts {

namespace {

const QString kVerb = QStringLiteral("doc_facts");

// The table's row order. Findings on one line are emitted in this order, which
// is the producer order doc_lint's INV-7 tiebreak consumes.
enum Row { RowInvariant = 0, RowMarkup, RowVersion, RowVerbArg };

struct Pending {
    int     line;
    int     row;
    QString kind;
    QString message;
};

// One line with every inline code span replaced by spaces. Spaces rather than
// deletion, so text on either side of a span never fuses into one token.
QStringList maskCodeSpans(const QStringList &lines,
                          const QVector<MarkdownScan::CodeSpan> &spans) {
    QStringList out = lines;
    for (const MarkdownScan::CodeSpan &s : spans) {
        for (int ln = s.startLine; ln <= s.endLine && ln < out.size(); ++ln) {
            QString &t = out[ln];
            const int from = ln == s.startLine ? qMax(0, s.startCol - s.delimLen) : 0;
            const int to   = ln == s.endLine ? qMin(int(t.size()), s.endCol + s.delimLen)
                                             : int(t.size());
            for (int c = from; c < to; ++c) t[c] = QLatin1Char(' ');
        }
    }
    return out;
}

QString spanContent(const QStringList &lines, const MarkdownScan::CodeSpan &s) {
    if (s.startLine == s.endLine)
        return lines.at(s.startLine).mid(s.startCol, s.endCol - s.startCol);
    QStringList parts;
    parts << lines.at(s.startLine).mid(s.startCol);
    for (int ln = s.startLine + 1; ln < s.endLine; ++ln) parts << lines.at(ln);
    parts << lines.at(s.endLine).left(s.endCol);
    return parts.join(QLatin1Char(' '));
}

// ---- verb_arg_unknown -------------------------------------------------------

// Top-level keys of one relaxed-JSON object spanning the WHOLE of `s`. Keys may
// be quoted or bare. Returns false when `s` is not exactly one balanced object.
bool objectKeys(const QString &s, QStringList *keys) {
    if (!s.startsWith(QLatin1Char('{'))) return false;
    int depth = 0;
    QChar quote;
    bool expectKey = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == quote) quote = QChar();
            continue;
        }
        if (c == QLatin1Char('{') || c == QLatin1Char('[')) {
            ++depth;
            expectKey = depth == 1 && c == QLatin1Char('{');
            continue;
        }
        if (c == QLatin1Char('}') || c == QLatin1Char(']')) {
            --depth;
            if (depth == 0) return i == s.size() - 1;
            if (depth < 0) return false;
            continue;
        }
        if (depth == 1 && c == QLatin1Char(',')) { expectKey = true; continue; }
        if (depth == 1 && expectKey && !c.isSpace()) {
            expectKey = false;
            QString key;
            int j = i;
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                const int close = s.indexOf(c, i + 1);
                if (close < 0) return false;
                key = s.mid(i + 1, close - i - 1);
                j = close + 1;
            } else {
                while (j < s.size() && (s.at(j).isLetterOrNumber() || s.at(j) == QLatin1Char('_')))
                    ++j;
                key = s.mid(i, j - i);
            }
            while (j < s.size() && s.at(j).isSpace()) ++j;
            if (key.isEmpty() || j >= s.size() || s.at(j) != QLatin1Char(':')) return false;
            keys->append(key);
            i = j;  // the ':' — the value is skipped by the depth tracking
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) quote = c;
    }
    return false;  // never closed
}

// `key:value` tokens separated by top-level whitespace; every token must have
// that shape, or the span is not a call.
bool tokenKeys(const QString &s, QStringList *keys) {
    static const QRegularExpression tok(QStringLiteral(R"(^([A-Za-z_][A-Za-z0-9_]*):\S)"));
    QStringList tokens;
    QString cur;
    int depth = 0;
    QChar quote;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (!quote.isNull()) {
            cur += c;
            if (c == QLatin1Char('\\') && i + 1 < s.size()) cur += s.at(++i);
            else if (c == quote) quote = QChar();
            continue;
        }
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) quote = c;
        else if (c == QLatin1Char('{') || c == QLatin1Char('[')) ++depth;
        else if (c == QLatin1Char('}') || c == QLatin1Char(']')) --depth;
        if (c.isSpace() && depth == 0) {
            if (!cur.isEmpty()) tokens << cur;
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.isEmpty()) tokens << cur;
    if (tokens.isEmpty()) return false;
    for (const QString &t : std::as_const(tokens)) {
        const QRegularExpressionMatch m = tok.match(t);
        if (!m.hasMatch()) return false;
        keys->append(m.captured(1));
    }
    return true;
}

}  // namespace

Result check(const QString &text, const QString &relPath, const Options &opts) {
    Result r;

    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    const QVector<bool> fence = MarkdownScan::fenceMask(lines);
    const QVector<MarkdownScan::CodeSpan> spans = MarkdownScan::codeSpans(lines, fence);
    const QStringList masked = maskCodeSpans(lines, spans);

    QList<Pending> out;
    const auto add = [&out](int line, int row, const QString &kind, const QString &msg) {
        out.append({line, row, kind, msg});
    };

    // ---- invariant_duplicate ----
    {
        static const QRegularExpression def(
            QStringLiteral("^ {0,3}") + SpecParse::invBulletAnchorPattern());
        QVector<bool> inInvariants(lines.size(), false);
        for (const MarkdownScan::Heading &h : MarkdownScan::headings(lines)) {
            if (!h.text.contains(QStringLiteral("Invariants"))) continue;
            for (int ln = h.line; ln <= h.endLine && ln <= lines.size(); ++ln)
                inInvariants[ln - 1] = true;
        }
        QHash<QString, int> firstAt;
        for (int i = 0; i < lines.size(); ++i) {
            if (!inInvariants.at(i) || fence.at(i)) continue;
            const QRegularExpressionMatch m = def.match(lines.at(i));
            if (!m.hasMatch()) continue;
            const QString id = m.captured(1);
            if (firstAt.contains(id))
                add(i + 1, RowInvariant, QStringLiteral("invariant_duplicate"),
                     QStringLiteral("%1 is already defined at line %2")
                         .arg(id).arg(firstAt.value(id)));
            else
                firstAt.insert(id, i + 1);
        }
    }

    // ---- leaked_markup ----
    {
        static const QRegularExpression tag(QStringLiteral(
            R"(</?(?:[A-Za-z_][A-Za-z0-9_.-]*:)?(invoke|function_calls|function_results|parameter)(?=[\s>/]))"));
        for (int i = 0; i < lines.size(); ++i) {
            if (fence.at(i)) continue;
            const QRegularExpressionMatch m = tag.match(masked.at(i));
            if (m.hasMatch())
                add(i + 1, RowMarkup, QStringLiteral("leaked_markup"),
                     QStringLiteral("tool-call markup <%1> in prose").arg(m.captured(1)));
        }
    }

    // ---- version_drift ----
    // A changelog is exempt: it names past versions by design.
    const bool changelog =
        QFileInfo(relPath).fileName().startsWith(QStringLiteral("CHANGELOG"));
    if (!opts.projectVersion.isEmpty() && !changelog) {
        static const QRegularExpression banner(
            QStringLiteral(R"(Version <strong>([0-9]+\.[0-9]+\.[0-9]+))"));
        static const QRegularExpression phrase(
            QStringLiteral(R"(current (?:version|release).*?\b([0-9]+\.[0-9]+\.[0-9]+))"),
            QRegularExpression::CaseInsensitiveOption);
        for (int i = 0; i < lines.size(); ++i) {
            if (fence.at(i)) continue;
            for (const QRegularExpression *re : {&banner, &phrase}) {
                auto it = re->globalMatch(masked.at(i));
                while (it.hasNext()) {
                    const QString claimed = it.next().captured(1);
                    ++r.versionClaimsChecked;
                    if (claimed != opts.projectVersion)
                        add(i + 1, RowVersion, QStringLiteral("version_drift"),
                             QStringLiteral("claims %1, the project version is %2")
                                 .arg(claimed, opts.projectVersion));
                }
            }
        }
    }

    // ---- verb_arg_unknown ----
    if (!opts.verbArgs.isEmpty()) {
        static const QRegularExpression head(
            QStringLiteral(R"(^(?:mcp__ants__)?([A-Za-z_][A-Za-z0-9_]*)\s+(\S.*)$)"));
        const auto checkCall = [&](int line, const QString &candidate) {
            const QRegularExpressionMatch m = head.match(candidate.trimmed());
            if (!m.hasMatch()) return;
            const QString verb = m.captured(1);
            const auto known = opts.verbArgs.constFind(verb);
            if (known == opts.verbArgs.constEnd()) return;
            const QString rest = m.captured(2).trimmed();
            QStringList keys;
            if (!objectKeys(rest, &keys)) {
                keys.clear();
                if (!tokenKeys(rest, &keys)) return;
            }
            ++r.verbCallsChecked;
            for (const QString &k : std::as_const(keys))
                if (!known->contains(k))
                    add(line, RowVerbArg, QStringLiteral("verb_arg_unknown"),
                         QStringLiteral("%1 takes no argument \"%2\"").arg(verb, k));
        };
        for (const MarkdownScan::CodeSpan &s : spans)
            checkCall(s.startLine + 1, spanContent(lines, s));
        // Fenced lines are read here and nowhere else: a fenced call example is
        // exactly what a caller copies.
        for (int i = 0; i < lines.size(); ++i)
            if (fence.at(i)) checkCall(i + 1, lines.at(i));
    }

    std::stable_sort(out.begin(), out.end(), [](const Pending &a, const Pending &b) {
        return a.line != b.line ? a.line < b.line : a.row < b.row;
    });
    int emission = 0;
    for (const Pending &p : std::as_const(out)) {
        DocFinding::Finding f;
        f.verb          = kVerb;
        f.kind          = p.kind;
        f.file          = relPath;
        f.line          = p.line;
        f.message       = p.message;
        f.autoFixable   = false;
        f.emissionIndex = emission++;
        r.findings.append(f);
    }
    return r;
}

}  // namespace DocFacts
