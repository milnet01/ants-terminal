// ANTS-5506 — doc_facts. See docfacts.h and docs/specs/ANTS-3663.md § 2.6.

#include "docfacts.h"
#include "markdownscan.h"

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
enum Row { RowCount = 0, RowInvariant, RowMarkup, RowVersion, RowVerbArg };

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

int leadingSpaces(const QString &s) {
    int n = 0;
    while (n < s.size() && s.at(n) == QLatin1Char(' ')) ++n;
    return n;
}

// ---- count_mismatch ---------------------------------------------------------

// True when `claim` holds exactly one number and that number is a cardinal:
// digits 2-99 or a word two-twelve. "one" and "1" are numbers but never a
// claim ("The one step:" states no count worth checking) — they still count
// toward the one-number rule, so "three problems and one other:" is a sum and
// not a claim of three.
bool singleCardinal(const QString &masked, int *value) {
    // A STANDALONE token: whitespace or line start before it, whitespace, `:`,
    // `,` or line end after it. Measured over this repo's docs, a bare \b
    // boundary read `INV-6`, `loop-2`, `Steps 1–3`, `(62 LoC)` and `~25` as
    // counts, and every one of those hits was false.
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|(?<=\s))([0-9]+|one|two|three|four|five|six|seven|eight|nine|ten|eleven|twelve)(?=$|[\s:,]))"),
        QRegularExpression::CaseInsensitiveOption);
    // A digit after a capitalised word is a LABEL — `Phase 7`, `Qt 6`,
    // `Step 3` — unless that word is a determiner, as in `The 3 steps`.
    static const QRegularExpression labelWord(QStringLiteral(R"(([A-Za-z]+)\s+$)"));
    static const QSet<QString> determiners = {
        QStringLiteral("the"),  QStringLiteral("these"), QStringLiteral("those"),
        QStringLiteral("all"),  QStringLiteral("its"),   QStringLiteral("our"),
        QStringLiteral("their"), QStringLiteral("both"), QStringLiteral("only"),
    };
    static const QHash<QString, int> words = {
        {QStringLiteral("one"), 1},
        {QStringLiteral("two"), 2},    {QStringLiteral("three"), 3},
        {QStringLiteral("four"), 4},   {QStringLiteral("five"), 5},
        {QStringLiteral("six"), 6},    {QStringLiteral("seven"), 7},
        {QStringLiteral("eight"), 8},  {QStringLiteral("nine"), 9},
        {QStringLiteral("ten"), 10},   {QStringLiteral("eleven"), 11},
        {QStringLiteral("twelve"), 12},
    };
    int n = 0;
    auto it = re.globalMatch(masked);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString tok = m.captured(1);
        int v = 0;
        if (tok.at(0).isDigit()) {
            bool ok = false;
            v = tok.toInt(&ok);
            if (!ok) continue;
            const QRegularExpressionMatch prev =
                labelWord.match(masked.left(m.capturedStart(1)));
            if (prev.hasMatch() && prev.captured(1).at(0).isUpper() &&
                !determiners.contains(prev.captured(1).toLower()))
                continue;
        } else {
            v = words.value(tok.toLower());
        }
        ++n;
        *value = v;
    }
    return n == 1 && *value >= 2 && *value <= 99;
}

const QRegularExpression &listItemRe() {
    static const QRegularExpression re(QStringLiteral(R"(^ *(?:([-*+])|[0-9]+([.)]))\s)"));
    return re;
}

// The marker's kind: the bullet character, or the ordered delimiter. A change
// of kind starts a new list (CommonMark § 5.3), so `- a` then `1. b` is two.
QString markerKind(const QRegularExpressionMatch &m) {
    return m.captured(1).isEmpty() ? QStringLiteral("ol") + m.captured(2) : m.captured(1);
}

// Top-level items of the list opening at `first`. A line indented deeper than
// the first item is a nested item or a continuation and is not counted; a
// line at its level that is not an item ends the list, and so does any line
// LEFT of it — where the lead-in is itself an item, that line is the parent
// list's next item, not one of this list's.
int countListItems(const QStringList &lines, int first) {
    const int base = leadingSpaces(lines.at(first));
    const QString kind = markerKind(listItemRe().match(lines.at(first)));
    int items = 0;
    for (int k = first; k < lines.size(); ++k) {
        const QString &l = lines.at(k);
        if (l.trimmed().isEmpty()) continue;
        const int ind = leadingSpaces(l);
        if (ind > base + 1) continue;
        if (ind < base) break;
        const QRegularExpressionMatch m = listItemRe().match(l);
        if (!m.hasMatch() || markerKind(m) != kind) break;
        ++items;
    }
    return items;
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

    // ---- count_mismatch ----
    // The CLAIM is the last sentence of the lead-in line: in `GUI users. Two new
    // files:` only the second sentence introduces the list. A numbered item's
    // own marker (`2. For each file:`) is never the claim — `2.` is not a
    // standalone token, and its `. ` reads as a sentence break anyway.
    // A line with no sentence break that continues a wrapped paragraph is not a
    // claim either — its sentence began on an earlier line, where any number
    // in it (`Qt 6`, `those six whether or not…`) describes something else.
    static const QRegularExpression marker(
        QStringLiteral(R"(^\s*(?:>\s*)*(?:[-*+]|[0-9]+[.)])\s+)"));
    static const QRegularExpression sentenceBreak(QStringLiteral(R"([.!?]\**\s+(?=\S))"));
    for (int i = 0; i < lines.size(); ++i) {
        if (fence.at(i)) continue;
        if (!lines.at(i).trimmed().endsWith(QLatin1Char(':'))) continue;
        QString t = masked.at(i);
        const bool isItem = marker.match(t).hasMatch();
        qsizetype cut = -1;
        for (auto sb = sentenceBreak.globalMatch(t); sb.hasNext();)
            cut = sb.next().capturedEnd();
        if (cut >= 0) {
            t = t.mid(cut);
        } else if (!isItem && i > 0 && !fence.at(i - 1) &&
                   !lines.at(i - 1).trimmed().isEmpty() &&
                   !lines.at(i).trimmed().startsWith(QLatin1Char('#'))) {
            continue;  // the tail of a wrapped paragraph
        }
        t = t.trimmed();
        int stated = 0;
        if (!singleCardinal(t, &stated)) continue;
        ++r.countClaimsChecked;
        int j = i + 1;
        while (j < lines.size() && lines.at(j).trimmed().isEmpty()) ++j;
        if (j >= lines.size() || fence.at(j) || !listItemRe().match(lines.at(j)).hasMatch())
            continue;  // a lead-in over prose or a table states no list count
        const int actual = countListItems(lines, j);
        if (actual != stated)
            add(i + 1, RowCount, QStringLiteral("count_mismatch"),
                 QStringLiteral("lead-in states %1, the list below it has %2 items")
                     .arg(stated).arg(actual));
    }

    // ---- invariant_duplicate ----
    {
        static const QRegularExpression def(
            QStringLiteral(R"(^ {0,3}-\s+\*\*(INV-\d+[a-z]?)\.?\*\*)"));
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
