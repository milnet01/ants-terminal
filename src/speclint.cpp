// ANTS-3662 — see speclint.h.

#include "speclint.h"

#include <algorithm>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringView>
#include <QVector>

#include "markdownscan.h"
#include "specparse.h"

namespace SpecLint {
namespace {

// The two tombstone forms, given as literal anchored regexes because "begins
// with an italic marker" is not parser-decidable — it leaves open whether any
// leading italic run counts, whether the id is constrained, and whether the
// separator is an em dash or a hyphen (spec INV-3). `—` is U+2014.
//
// `*moved to <ID>*` is observed a dozen times in ANTS-3636. `*withdrawn — …*`
// had ZERO corpus occurrences when this shipped: it is introduced by the spec,
// because /write-spec sanctions withdrawal ("Withdraw or annotate; never
// renumber") while defining no syntax for it. Its fixture row exists so the
// invented form cannot ship unexercised.
bool isTombstone(const QString &body) {
    static const QRegularExpression movedRe(
        QStringLiteral(R"(^\*moved to ([A-Z]+-\d+)\*)"));
    static const QRegularExpression withdrawnRe(
        QStringLiteral(R"(^\*withdrawn — (.+?)\*)"));
    const QString b = body.trimmed();
    return movedRe.match(b).hasMatch() || withdrawnRe.match(b).hasMatch();
}

// A fixed vocabulary, not a shape test: `QProcess` and `docs/standards/specs.md`
// are code spans too, and a rule like "contains a slash or a dash" fires on
// both (spec § 2.1).
const QSet<QString> &commandWords() {
    static const QSet<QString> v = {
        QStringLiteral("grep"),   QStringLiteral("rg"),      QStringLiteral("git"),
        QStringLiteral("ls"),     QStringLiteral("find"),    QStringLiteral("ctest"),
        QStringLiteral("cmake"),  QStringLiteral("ninja"),   QStringLiteral("make"),
        QStringLiteral("pytest"), QStringLiteral("python"),  QStringLiteral("python3"),
        QStringLiteral("node"),   QStringLiteral("npm"),     QStringLiteral("bash"),
        QStringLiteral("sh"),     QStringLiteral("sed"),     QStringLiteral("awk"),
        QStringLiteral("wc"),     QStringLiteral("cat"),     QStringLiteral("head"),
        QStringLiteral("tail"),   QStringLiteral("diff"),    QStringLiteral("jq"),
        QStringLiteral("curl"),   QStringLiteral("test"),
    };
    return v;
}

bool isCommandSpan(const QString &content) {
    QString s = content.trimmed();
    if (s.startsWith(QLatin1String("$ "))) s = s.mid(2).trimmed();
    const int sp = s.indexOf(QRegularExpression(QStringLiteral(R"(\s)")));
    return commandWords().contains(sp < 0 ? s : s.left(sp));
}

// True when the clause contains a command-shaped code span and no alphanumeric
// character follows the LAST such span — § 1e's "states nothing it should
// return", read literally. Trailing punctuation is not an expectation; a digit
// or letter is.
//
// Deliberately NOT keyed on the `→` arrow: that is the corpus's habit, not a
// documented convention (docs/standards/specs.md does not mention it), so a
// check keyed on it would report every clause written before the habit formed.
bool isCommandWithoutExpectation(const QString &clause) {
    if (clause.isEmpty()) return false;
    const QStringList lines = clause.split(QLatin1Char('\n'));
    const QVector<bool> noFence(lines.size(), false);

    // Flat offset of each line's first character, so a span's (line, col) can be
    // compared against the rest of the clause as one string.
    QVector<int> lineStart(lines.size(), 0);
    for (int i = 1; i < lines.size(); ++i)
        lineStart[i] = lineStart[i - 1] + lines[i - 1].size() + 1;

    int lastEnd = -1;
    const auto spans = MarkdownScan::codeSpans(lines, noFence);
    for (const auto &s : spans) {
        if (s.startLine < 0 || s.startLine >= lines.size()) continue;
        if (s.endLine < 0 || s.endLine >= lines.size()) continue;
        QString content;
        if (s.startLine == s.endLine) {
            content = lines[s.startLine].mid(s.startCol, s.endCol - s.startCol);
        } else {
            content = lines[s.startLine].mid(s.startCol);
            for (int l = s.startLine + 1; l < s.endLine; ++l)
                content += QLatin1Char(' ') + lines[l];
            content += QLatin1Char(' ') + lines[s.endLine].left(s.endCol);
        }
        if (isCommandSpan(content))
            lastEnd = lineStart[s.endLine] + s.endCol + s.delimLen;
    }
    if (lastEnd < 0) return false;

    const QString tail = clause.mid(lastEnd);
    for (const QChar &c : tail)
        if (c.isLetterOrNumber()) return false;
    return true;
}

// GFM row cells, honouring the `\|` escape — a spec that TABULATES table shapes
// writes one, and splitting naively turns its single cell into nine.
QStringList tableCells(const QString &line) {
    QString s = line.trimmed();
    if (s.startsWith(QLatin1Char('|'))) s = s.mid(1);
    if (s.endsWith(QLatin1Char('|')) &&
        !s.endsWith(QLatin1String("\\|")))
        s.chop(1);

    QStringList cells;
    QString cur;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && i + 1 < s.size() &&
            s.at(i + 1) == QLatin1Char('|')) {
            cur += QLatin1String("\\|");
            ++i;
        } else if (c == QLatin1Char('|')) {
            cells.append(cur.trimmed());
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.append(cur.trimmed());
    return cells;
}

bool isTableRow(const QString &line) {
    const QString s = line.trimmed();
    return s.startsWith(QLatin1Char('|')) && s.size() > 1;
}

bool isDelimiterRow(const QString &line) {
    static const QRegularExpression re(
        QStringLiteral(R"(^\|?[\s:|-]*-[\s:|-]*\|?$)"));
    const QString s = line.trimmed();
    return isTableRow(line) && re.match(s).hasMatch();
}

QString collapseWs(const QString &s) {
    static const QRegularExpression ws(QStringLiteral(R"(\s+)"));
    return s.trimmed().replace(ws, QStringLiteral(" "));
}

}  // namespace

QSet<int> invariantNumbers(const QString &text) {
    static const QRegularExpression anchorRe(
        QStringLiteral(R"(^ {0,3}(?:-\s+\*\*|\|\s*)INV-([0-9]+)[a-z]?)"),
        QRegularExpression::MultilineOption);
    QSet<int> out;
    auto it = anchorRe.globalMatch(text);
    while (it.hasNext()) out.insert(it.next().captured(1).toInt());
    return out;
}

QStringList parseRequiredSections(const QString &standardText) {
    const QStringList lines = standardText.split(QLatin1Char('\n'));
    int marker = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].contains(QLatin1String("<!-- required-sections -->"))) {
            marker = i;
            break;
        }
    }
    if (marker < 0) return {};

    QStringList out;
    QChar closer;
    for (int i = marker + 1; i < lines.size(); ++i) {
        if (closer.isNull()) {
            closer = MarkdownScan::fenceOpenerChar(lines[i]);
            // A non-blank line before the fence means the marker was not
            // introducing one: no block, so no list (the skip signal).
            if (closer.isNull() && !lines[i].trimmed().isEmpty()) return {};
            continue;
        }
        if (MarkdownScan::fenceOpenerChar(lines[i]) == closer) break;
        const QString h = lines[i].trimmed();
        if (!h.isEmpty()) out.append(h);
    }
    return out;
}

Result check(const QString &text, const QString &relPath,
             const Options &opts) {
    Result r;
    const QStringList lines = text.split(QLatin1Char('\n'));
    r.lineCount = lines.size();
    if (r.lineCount > 0 && lines.last().isEmpty()) --r.lineCount;

    const QVector<bool> fence = MarkdownScan::fenceMask(lines);

    int emitted = 0;
    const auto add = [&](const QString &kind, int line, const QString &msg,
                         bool autoFixable = false) {
        if (r.findings.size() >= opts.maxFindings) {
            r.truncated = true;
            return;
        }
        DocFinding::Finding f;
        f.verb          = QStringLiteral("spec_lint");
        f.kind          = kind;
        f.file          = relPath;
        f.line          = line;
        f.message       = msg;
        f.autoFixable   = autoFixable;
        f.emissionIndex = emitted++;
        r.findings.append(f);
    };

    // --- headings (fence-aware) ------------------------------------------
    static const QRegularExpression headingRe(
        QStringLiteral(R"(^ {0,3}(#{1,6})\s+(\S.*?)\s*$)"));
    // The Invariants section's line range, mirroring parseSpecBody's own
    // boundaries (its heading regex, then the next `## `). The anchor scan below
    // is confined to it: an `- **INV-4**` written in § 7 as an EXAMPLE is prose,
    // and counting it would invent both an invariant and an id gap.
    //
    // ANTS-4115 — strict-first, then a heading whose text merely CONTAINS the
    // word (`## 5. Correctness invariants`). Mirroring parseSpecBody is the whole
    // point of this block: a lint whose section boundaries disagree with the
    // parser's reads one document and reports on another.
    static const QRegularExpression invHdrRe(
        QStringLiteral(R"(^ {0,3}#{2,3}\s+(?:\d+\.\s+)?[Ii]nvariants\b)"));
    static const QRegularExpression invHdrLooseRe(
        QStringLiteral(R"(^ {0,3}#{2,3}\s+(?:\d+\.\s+)?.*\b[Ii]nvariants\b)"));
    static const QRegularExpression nextH2Re(QStringLiteral(R"(^ {0,3}##\s+\S)"));
    int invHdrStrict = -1, invHdrLoose = -1;
    QStringList headingLines;   // normalised `## N. Name`
    for (int i = 0; i < lines.size(); ++i) {
        if (i < fence.size() && fence[i]) continue;
        const auto m = headingRe.match(lines[i]);
        if (m.hasMatch())
            headingLines.append(collapseWs(m.captured(1) + QLatin1Char(' ') +
                                           m.captured(2)));
        if (invHdrStrict < 0 && invHdrRe.match(lines[i]).hasMatch())
            invHdrStrict = i;
        if (invHdrLoose < 0 && invHdrLooseRe.match(lines[i]).hasMatch())
            invHdrLoose = i;
    }
    const int invHdrLine = invHdrStrict >= 0 ? invHdrStrict : invHdrLoose;
    int invStart = invHdrLine >= 0 ? invHdrLine + 1 : -1;
    int invEnd   = lines.size();
    for (int i = invStart; invStart >= 0 && i < lines.size(); ++i) {
        if (i < fence.size() && fence[i]) continue;
        if (nextH2Re.match(lines[i]).hasMatch()) { invEnd = i; break; }
    }

    // --- missing_section (gated on an injected list) ----------------------
    // `line:0` is document scope per ANTS-3664 § 2.1, which is exactly what
    // "this document lacks a section" means — the heading scan has no entry for
    // a heading that is absent.
    if (!opts.requiredSections.isEmpty()) {
        r.sectionsChecked = true;
        const QSet<QString> present(headingLines.begin(), headingLines.end());
        for (const QString &req : opts.requiredSections) {
            const QString want = collapseWs(req);
            if (!present.contains(want))
                add(QStringLiteral("missing_section"), 0,
                    QStringLiteral("required section is absent: %1").arg(want));
        }
    }

    // --- INV-N anchor lines ----------------------------------------------
    // parseSpecBody supplies neither line numbers nor sections nor loop-log
    // rows, so every `line` on this verb's findings comes from this scan.
    // ANTS-4107 — `[a-z]?` so a sub-lettered id anchors like any other.
    static const QRegularExpression bulletAnchorRe(
        QStringLiteral(R"(^ {0,3}-\s+\*\*(INV-\d+[a-z]?)\.?\*\*)"));
    static const QRegularExpression rowAnchorRe(
        QStringLiteral(R"(^ {0,3}\|\s*(INV-\d+[a-z]?)\s*\|)"));
    QHash<QString, int> anchorLine;   // id -> 1-based line, first occurrence
    for (int i = qMax(invStart, 0); invStart >= 0 && i < invEnd; ++i) {
        if (i < fence.size() && fence[i]) continue;
        auto m = bulletAnchorRe.match(lines[i]);
        if (!m.hasMatch()) m = rowAnchorRe.match(lines[i]);
        if (m.hasMatch() && !anchorLine.contains(m.captured(1)))
            anchorLine.insert(m.captured(1), i + 1);
    }

    // --- invariant_no_test / command_test_no_expectation -------------------
    // The ANCHOR SCAN is the authority on which invariants EXIST; the parser is
    // the authority on what each one says. They are separate because the
    // parser's table branch requires three non-empty cells, so a row whose
    // test-surface cell is empty — the exact defect `invariant_no_test` names —
    // does not match and is returned by nothing. Trusting the parser's list
    // alone would therefore make the check blind in the table form AND invent an
    // id gap where the dropped row was (INV-2, INV-3).
    const QJsonObject parsed = SpecParse::parseSpecBody(text);
    QHash<QString, QJsonObject> parsedById;
    for (const auto &v : parsed.value(QStringLiteral("invariants")).toArray()) {
        const QJsonObject o = v.toObject();
        parsedById.insert(o.value(QStringLiteral("id")).toString(), o);
    }

    QSet<QString> allIds;
    for (auto it = anchorLine.constBegin(); it != anchorLine.constEnd(); ++it)
        allIds.insert(it.key());
    for (auto it = parsedById.constBegin(); it != parsedById.constEnd(); ++it)
        allIds.insert(it.key());

    // ANTS-4107 — the checks below walk the ID STRINGS; only the gap scan
    // reduces them to numbers. Rebuilding `INV-%1` from an int silently dropped
    // every sub-lettered invariant from every check, so one could ship with no
    // test surface at all and pass the lint — and a /cold-eyes split into 3 and
    // 3b is exactly how a sub-lettered id comes to exist, which put the blind
    // spot precisely where a contract had just been divided in two.
    struct InvId {
        int     n;
        QString letter;
        QString id;
    };
    static const QRegularExpression invIdRe(
        QStringLiteral(R"(^INV-([0-9]+)([a-z]?)$)"));
    QVector<InvId> ids;
    for (const QString &id : allIds) {
        const auto m = invIdRe.match(id);
        if (!m.hasMatch()) continue;
        ids.append({m.captured(1).toInt(), m.captured(2), id});
    }
    std::sort(ids.begin(), ids.end(), [](const InvId &a, const InvId &b) {
        return a.n != b.n ? a.n < b.n : a.letter < b.letter;
    });

    for (const InvId &e : ids) {
        const QString     id = e.id;
        const QJsonObject o  = parsedById.value(id);
        const int         line = anchorLine.value(id, 0);

        // A withdrawn or moved invariant has nothing left to test. Without the
        // exemption this check fires against every tombstone in the corpus —
        // docs/specs/ANTS-3663.md carries two today.
        if (isTombstone(o.value(QStringLiteral("body")).toString())) continue;

        // Blank counts as absent, and that is not defensive: the bullet branch
        // OMITS the key when there is no clause, while the table branch's
        // three-cell regex captures a whitespace-only cell as a present-but-
        // blank surface. Keying on the key's presence alone would therefore
        // report the bullet form and silently pass the table form.
        const QString clause =
            o.value(QStringLiteral("test_surface")).toString().trimmed();
        if (clause.isEmpty()) {
            add(QStringLiteral("invariant_no_test"), line,
                QStringLiteral("%1 carries no test-surface clause").arg(id));
            continue;
        }

        // A CANDIDATE, never a verdict: "is this clause a command?" is a
        // heuristic, and a manual recipe or a bare test-file path is a
        // legitimate surface with nothing to state. Never autoFixable, and this
        // engine runs no subprocess — /write-spec Step 3 owns executing a
        // clause, at write time, where a failure is free (INV-4).
        //
        // Execution splits by artefact, and only one half moved (ANTS-4108):
        // a PATTERN stated as a ```regex pcre2 fence with an | input |
        // expected | table beside it is run by the spec_conformance verb,
        // which needs no subprocess because applying a pattern to a string
        // cannot reach the filesystem. A COMMAND clause — this branch — still
        // has no runtime owner but /write-spec Step 3, and fenced FIXTURES
        // have none at all until ANTS-4127 settles an interpreter and sandbox.
        if (isCommandWithoutExpectation(clause))
            add(QStringLiteral("command_test_no_expectation"), line,
                QStringLiteral("%1's test clause is a command but states "
                               "nothing it should return (candidate)").arg(id));
    }

    // --- invariant_id_gap --------------------------------------------------
    // The id set includes tombstones. Excluding them first is the mistake this
    // check guards against: it would report every moved or withdrawn invariant
    // as a gap (INV-3).
    if (!ids.isEmpty()) {
        QSet<int> have;
        for (const InvId &e : ids) have.insert(e.n);
        // From the document's OWN MINIMUM, not from 1. A spec whose sequence
        // legitimately starts above 1 — docs/specs/ANTS-1358.md opens at INV-14
        // because it continues an earlier spec's numbering — has an offset
        // origin, not thirteen gaps. Anchoring at 1 reported exactly that
        // against the corpus (measured: it was most of the 109 id-gap findings
        // in the first calibration run), and § 1e asks for gaps in "a doc's own
        // numbered ids".
        int lo = ids.first().n, hi = ids.first().n;
        for (const InvId &e : ids) { lo = qMin(lo, e.n); hi = qMax(hi, e.n); }
        for (int n = lo; n <= hi; ++n) {
            if (have.contains(n)) continue;
            // ANTS-4110 — a number a SIBLING spec owns is not a gap. On a
            // project that numbers invariants once across the corpus, every id
            // living in a neighbouring document reads as one here, and the two
            // repairs the finding invites are both forbidden by the standard it
            // is meant to enforce: renumber (ids are permanent) or tombstone ids
            // that were never in this file. Suppressed rather than filtered
            // afterwards, so the count below is the whole story.
            if (opts.siblingInvNumbers.contains(n)) {
                ++r.idGapsSuppressed;
                continue;
            }
            // A missing invariant has no line of its own, so the finding
            // anchors on the bullet FOLLOWING the gap.
            int line = 0;
            for (const InvId &e : ids) {
                if (e.n <= n) continue;
                const int cand = anchorLine.value(e.id, 0);
                if (cand > 0) { line = cand; break; }
            }
            add(QStringLiteral("invariant_id_gap"), line,
                QStringLiteral("INV-%1 is missing from the id sequence with no "
                               "tombstone").arg(n));
        }
    }

    // --- loop_row_no_outcome ----------------------------------------------
    // A loop log is a GFM table whose FIRST HEADER CELL is exactly `Loop`; the
    // outcome is the row's LAST cell. Four incompatible column layouts are in
    // use across this corpus (spec § 2.2) and the prose column is last in all
    // four, so position matches every shape where a column NAME matches one.
    for (int i = 0; i + 1 < lines.size(); ++i) {
        if (i < fence.size() && fence[i]) continue;
        if (!isTableRow(lines[i]) || isDelimiterRow(lines[i])) continue;
        if (!isDelimiterRow(lines[i + 1])) continue;
        const QStringList hdr = tableCells(lines[i]);
        if (hdr.isEmpty() || hdr.first() != QLatin1String("Loop")) continue;

        for (int j = i + 2; j < lines.size(); ++j) {
            if (j < fence.size() && fence[j]) break;
            if (!isTableRow(lines[j])) break;
            const QStringList cells = tableCells(lines[j]);
            if (cells.size() < 2 || !cells.last().isEmpty()) continue;
            add(QStringLiteral("loop_row_no_outcome"), j + 1,
                QStringLiteral("cold-eyes loop row %1 records no outcome")
                    .arg(cells.first()));
        }
    }

    return r;
}

}  // namespace SpecLint
