// ANTS-1963 — implementation of the spec_log pure transforms.
// See speclog.h + docs/specs/ANTS-1963.md.

#include "speclog.h"

#include "markdownscan.h"
// ANTS-3785 — the header-field extent rule lives in SpecParse so the reader
// and this writer share one definition of where a field ends.
#include "specparse.h"

#include <QRegularExpression>
#include <QStringList>

namespace SpecLog {

namespace {

EditResult fail(const QString &code, const QString &error) {
    EditResult r;
    r.ok = false;
    r.code = code;
    r.error = error;
    return r;
}

// ANTS-3603 — the fence primitives now live in MarkdownScan (markdownscan.h).
// Bring fenceOpenerChar into this namespace so findSectionHeading below calls
// it unqualified, exactly as it did before the hoist.
using MarkdownScan::fenceOpenerChar;

// A `## ` section boundary: exactly two hashes + space (`### `+ never
// ends a `## ` section).
bool isLevel2Heading(const QString &line) {
    return line.startsWith(QStringLiteral("## ")) &&
           !line.startsWith(QStringLiteral("### "));
}

// Find the 0-based line index of the FIRST `## ` heading whose text
// contains `keyword` (case-insensitive substring), skipping fenced
// regions. Returns -1 when none.
int findSectionHeading(const QStringList &lines, const QString &keyword) {
    QChar openFence;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            const QChar c = fenceOpenerChar(line);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(line);
        if (!opener.isNull()) { openFence = opener; continue; }
        if (isLevel2Heading(line) &&
            line.contains(keyword, Qt::CaseInsensitive)) {
            return i;
        }
    }
    return -1;
}

// ANTS-3651 — how MANY level-2 headings carry the keyword. Same fence-aware
// walk as findSectionHeading, which returns only the first: a file that
// already holds two is ambiguous, and appending to the first silently deepens
// a structural defect its author has not noticed. Counting is the cheapest way
// for the caller to tell "found it" from "found several".
int countSectionHeadings(const QStringList &lines, const QString &keyword) {
    QChar openFence;
    int n = 0;
    for (const QString &line : lines) {
        if (!openFence.isNull()) {
            const QChar c = fenceOpenerChar(line);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(line);
        if (!opener.isNull()) { openFence = opener; continue; }
        if (isLevel2Heading(line) &&
            line.contains(keyword, Qt::CaseInsensitive)) {
            ++n;
        }
    }
    return n;
}

// Given the heading index `hdrIdx`, find the end of that section: the
// first `## ` boundary after it (fence-skipped, `###`+ inert) or EOF.
// Returns the 0-based index of the boundary line (== lines.size() at EOF).
int sectionEnd(const QStringList &lines, int hdrIdx) {
    QChar openFence;
    for (int i = hdrIdx + 1; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            const QChar c = fenceOpenerChar(line);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(line);
        if (!opener.isNull()) { openFence = opener; continue; }
        if (isLevel2Heading(line)) return i;
    }
    return lines.size();
}

// Insert `bullet` (a single rendered line, no trailing newline) at the
// end of the section that starts at `hdrIdx`, i.e. after the section's
// last non-blank content line and before the next `## ` boundary.
// Returns {newContent, 1-based-line-of-bullet}.
struct InsertResult { QString content; int line = -1; };

InsertResult insertAtSectionEnd(QStringList lines, int hdrIdx,
                                const QString &bullet,
                                bool fileEndedWithNewline) {
    const int end = sectionEnd(lines, hdrIdx);
    // Walk back from `end` to find the last non-blank content line of the
    // section; insert immediately after it.
    int insertAfter = hdrIdx;  // at minimum, after the heading
    for (int i = end - 1; i > hdrIdx; --i) {
        if (!lines.at(i).trimmed().isEmpty()) { insertAfter = i; break; }
    }
    // Insert position is insertAfter+1 (a new line entry).
    const int pos = insertAfter + 1;
    lines.insert(pos, bullet);
    InsertResult r;
    r.line = pos + 1;  // 1-based
    r.content = lines.join(QLatin1Char('\n'));
    if (fileEndedWithNewline) r.content += QLatin1Char('\n');
    return r;
}

// Split into lines for editing; remember whether the source ended in a
// trailing newline so we can restore it exactly (byte-fidelity outside
// the edit).
QStringList toLines(const QString &content, bool &endedWithNewline) {
    endedWithNewline = content.endsWith(QLatin1Char('\n'));
    QString body = content;
    if (endedWithNewline) body.chop(1);  // drop the single trailing \n
    return body.split(QLatin1Char('\n'));
}

}  // namespace

EditResult setStatus(const QString &content, const QString &newStatus,
                     bool preserveBody) {
    bool ewn = false;
    QStringList lines = toLines(content, ewn);

    // ANTS-3785 — replace the field's WHOLE extent, not just its first line.
    // A Status that wraps is the common case (49 of 172 specs), and rewriting
    // only the opener left the continuations behind as an orphaned paragraph
    // while still reporting success. SpecParse owns where a field ends so the
    // reader and this writer cannot disagree about it.
    const SpecParse::FieldExtent e =
        SpecParse::headerField(lines, QStringLiteral("Status"));
    if (e.found()) {
        const int end = e.line + e.lineCount;  // exclusive
        // ANTS-4136 — `preserveBody` keeps the continuation lines and
        // rewrites only the opener. The reported loss was a wrapped Status
        // whose review history lived on those lines, so keeping them turns
        // the documented recovery (re-paste from `previous_status`) into the
        // default. Deliberately a LINE rule rather than a prose-splitting
        // heuristic: there is no delimiter this corpus agrees on between the
        // state word and the prose after it, and guessing one would drop text
        // on the shapes it guessed wrong — the very failure being fixed.
        if (!preserveBody) {
            for (int k = end - 1; k > e.line; --k) lines.removeAt(k);
        }
        lines[e.line] = QStringLiteral("**Status:** ") + newStatus;

        EditResult r;
        r.ok = true;
        // ANTS-4114 — report the value being replaced. This verb imposes no
        // Status vocabulary (it writes `newStatus` verbatim) and cannot: the
        // permitted set is stated as prose in each project's own standard, in
        // shapes that do not share a grammar. The value already in the file is
        // the one project-agnostic evidence of that vocabulary, so hand it back.
        r.previousValue = e.value;
        r.content = lines.join(QLatin1Char('\n'));
        if (ewn) r.content += QLatin1Char('\n');
        // 1-based line of the rewritten Status line (ANTS-1963 INV-10's shape;
        // FieldExtent::line is 0-based).
        r.line = e.line + 1;
        return r;
    }

    return fail(QStringLiteral("unrecognised_format"),
                QStringLiteral("spec_log: no \"**Status:**\" line found "
                               "(set_status needs one to rewrite)"));
}

// ANTS-4353 — split a markdown table row into trimmed cells.
static QStringList slRowCells(const QString &line) {
    QString t = line.trimmed();
    if (t.startsWith(QLatin1Char('|'))) t = t.mid(1);
    if (t.endsWith(QLatin1Char('|')))   t.chop(1);
    QStringList out;
    for (const QString &c : t.split(QLatin1Char('|'))) out.append(c.trimmed());
    return out;
}

// A `|---|---|` separator, which is what makes the block above it a HEADER
// rather than a row that happens to contain pipes.
static bool slIsTableSeparator(const QString &line) {
    const QString t = line.trimmed();
    if (!t.startsWith(QLatin1Char('|'))) return false;
    for (const QString &c : slRowCells(line)) {
        if (c.isEmpty()) return false;
        for (const QChar ch : c) {
            if (ch != QLatin1Char('-') && ch != QLatin1Char(':')) return false;
        }
    }
    return true;
}

// The leading loop NUMBER of a data row, or -1. `4-tail` and `4-merge` are
// real labels in this corpus (a row hung off loop 4 that no review produced),
// so the number is the leading digits and the suffix is ignored.
static int slRowLoopNumber(const QString &line) {
    const QStringList cells = slRowCells(line);
    if (cells.isEmpty()) return -1;
    QString lead;
    for (const QChar ch : cells.first()) {
        if (ch.isDigit()) lead.append(ch);
        else if (!lead.isEmpty()) break;
    }
    bool ok = false;
    const int n = lead.toInt(&ok);
    return ok ? n : -1;
}

EditResult appendLoop(const QString &content, const QString &label,
                      const QString &body, const QStringList &cells) {
    bool ewn = false;
    QStringList lines = toLines(content, ewn);
    const QString bullet = QStringLiteral("- **") + label +
                           QStringLiteral("** — ") + body;

    // ANTS-3651 — refuse an ambiguous file rather than guessing at it. Two
    // loop-log sections means an earlier write already went wrong (that is
    // how ANTS-3636 ended up with loops ordered 1-6, 8, 9, then 7 under a
    // duplicate heading), and appending to whichever comes first buries the
    // problem one row deeper. The author has to see it to repair it.
    if (countSectionHeadings(lines,
                             QStringLiteral("Cold-eyes loop log")) > 1) {
        return fail(QStringLiteral("unrecognised_format"),
                    QStringLiteral(
                        "spec_log: this file holds more than one "
                        "\"## Cold-eyes loop log\" section, so append_loop "
                        "cannot tell which one the row belongs in. Merge them "
                        "into a single section (keeping the loops in order) "
                        "and retry — appending to one of them would leave the "
                        "log split and out of order."));
    }

    const int hdr = findSectionHeading(
        lines, QStringLiteral("Cold-eyes loop log"));
    if (hdr >= 0) {
        // ANTS-4364 — does this section hold a TABLE? The shipped spec
        // skeleton's loop log is one, while this verb only ever wrote a
        // bullet — so on a conforming spec the verb was unusable, and
        // `review-contract` tells sessions outright not to reach for it.
        // That is self-reinforcing: a verb the governing skill says to avoid
        // gets no usage, so the mismatch never becomes pressure to fix.
        int sectionEnd = lines.size();
        for (int i = hdr + 1; i < lines.size(); ++i) {
            if (lines.at(i).startsWith(QStringLiteral("## "))) {
                sectionEnd = i;
                break;
            }
        }
        int headerLine = -1, sepLine = -1;
        for (int i = hdr + 1; i + 1 < sectionEnd && i + 1 < lines.size(); ++i) {
            if (lines.at(i).trimmed().startsWith(QLatin1Char('|')) &&
                slIsTableSeparator(lines.at(i + 1))) {
                headerLine = i;
                sepLine    = i + 1;
                break;
            }
        }

        if (headerLine >= 0) {
            const QStringList header = slRowCells(lines.at(headerLine));
            if (cells.isEmpty()) {
                EditResult r;
                r.code = QStringLiteral("format_mismatch");
                r.error = QStringLiteral(
                    "spec_log: this spec's loop log is a TABLE (columns: %1), "
                    "so append_loop needs `cells` — one string per column, in "
                    "that order. `loop_label`/`body` render a BULLET, which "
                    "would corrupt the table. Refusing rather than writing it.")
                        .arg(header.join(QStringLiteral(" | ")));
                return r;
            }
            if (cells.size() != header.size()) {
                EditResult r;
                r.code = QStringLiteral("column_mismatch");
                r.error = QStringLiteral(
                    "spec_log: the loop log has %1 columns (%2) but `cells` "
                    "carries %3")
                        .arg(header.size())
                        .arg(header.join(QStringLiteral(" | ")))
                        .arg(cells.size());
                return r;
            }

            // Bound the contiguous data rows below the separator.
            int firstData = -1, lastData = -1;
            for (int i = sepLine + 1; i < sectionEnd && i < lines.size(); ++i) {
                if (!lines.at(i).trimmed().startsWith(QLatin1Char('|'))) {
                    if (firstData >= 0) break;   // the table ended
                    if (lines.at(i).trimmed().isEmpty()) continue;
                    break;
                }
                if (firstData < 0) firstData = i;
                lastData = i;
            }

            // ANTS-4353 — INFER the direction rather than assuming it. Loop
            // logs run in opposite orders across specs in one corpus (this
            // project has both), so "append at the end" means opposite ends
            // in different files. A row at the wrong end reads as a different
            // loop's result, and a checker that only balances per-row tallies
            // passes the corruption.
            QString rowOrder = QStringLiteral("ambiguous");
            int insertAt = (lastData >= 0) ? lastData + 1 : sepLine + 1;
            if (firstData >= 0 && lastData > firstData) {
                const int a = slRowLoopNumber(lines.at(firstData));
                const int b = slRowLoopNumber(lines.at(lastData));
                if (a >= 0 && b >= 0 && a != b) {
                    if (a < b) {
                        rowOrder = QStringLiteral("oldest_first");
                        insertAt = lastData + 1;
                    } else {
                        rowOrder = QStringLiteral("newest_first");
                        insertAt = firstData;
                    }
                }
            }

            QStringList escaped;
            for (const QString &c : cells) {
                QString e = c;
                e.replace(QLatin1Char('|'), QStringLiteral("\\|"));
                e.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
                escaped.append(e);
            }
            const QString row = QStringLiteral("| ") +
                                escaped.join(QStringLiteral(" | ")) +
                                QStringLiteral(" |");
            lines.insert(insertAt, row);

            EditResult r;
            r.ok       = true;
            r.content  = lines.join(QLatin1Char('\n'));
            if (ewn) r.content += QLatin1Char('\n');
            r.line     = insertAt + 1;   // 1-based
            r.rowShape = QStringLiteral("table");
            r.rowOrder = rowOrder;
            return r;
        }

        const auto ins = insertAtSectionEnd(lines, hdr, bullet, ewn);
        EditResult r;
        r.ok = true;
        r.content = ins.content;
        r.line = ins.line;
        r.rowShape = QStringLiteral("bullet");
        return r;
    }
    // No section — append a new `## Cold-eyes loop log` heading + the
    // bullet at EOF (repaired, not refused). `line` is the bullet line.
    QStringList out = lines;
    // Ensure a blank-line separator before the new heading.
    if (!out.isEmpty() && !out.last().trimmed().isEmpty())
        out.append(QString());
    out.append(QStringLiteral("## Cold-eyes loop log"));
    out.append(QString());
    out.append(bullet);
    EditResult r;
    r.ok = true;
    r.content = out.join(QLatin1Char('\n'));
    if (ewn) r.content += QLatin1Char('\n');
    r.line = out.size();  // bullet is the last line
    r.rowShape = QStringLiteral("bullet");
    return r;
}

EditResult appendInv(const QString &content, const QString &invId,
                     const QString &body, const QString &test) {
    bool ewn = false;
    QStringList lines = toLines(content, ewn);

    const int hdr = findSectionHeading(
        lines, QStringLiteral("Invariants"));
    if (hdr < 0) {
        return fail(QStringLiteral("unrecognised_format"),
                    QStringLiteral("spec_log: no Invariants section found"));
    }
    // Duplicate guard: refuse if `invId` already appears as an INV
    // bullet anywhere in the file. The bullet form is
    // `- **INV-N** —` / `- **INV-N.** —` (specs.md § 3.5).
    static const QRegularExpression invBulletRe(
        QStringLiteral("^-\\s+\\*\\*(INV-[0-9]+)"));
    for (const QString &line : lines) {
        const auto m = invBulletRe.match(line);
        if (m.hasMatch() && m.captured(1) == invId) {
            return fail(QStringLiteral("bad_args"),
                        QStringLiteral("spec_log: %1 already present "
                                       "(append_inv never renumbers / "
                                       "duplicates)").arg(invId));
        }
    }

    QString bullet = QStringLiteral("- **") + invId +
                     QStringLiteral("** — ") + body +
                     QStringLiteral(".");
    if (!test.isEmpty())
        bullet += QStringLiteral(" *Test:* ") + test + QStringLiteral(".");

    const auto ins = insertAtSectionEnd(lines, hdr, bullet, ewn);
    EditResult r;
    r.ok = true;
    r.content = ins.content;
    r.line = ins.line;
    return r;
}

}  // namespace SpecLog
