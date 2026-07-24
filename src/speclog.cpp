// ANTS-1963 — implementation of the spec_log pure transforms.
// See speclog.h + docs/specs/ANTS-1963.md.

#include "speclog.h"

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

const QRegularExpression &fenceRe() {
    // ^ {0,3}(```|~~~) — same fence rule the mcp-feedback-files.md
    // parser contract defines (ANTS-1963 § 2.3). Space-only indent per
    // CommonMark; a tab must not open a fence (ANTS-3598).
    static const QRegularExpression re(
        QStringLiteral("^ {0,3}(```|~~~)"));
    return re;
}

QChar fenceOpenerChar(const QString &line) {
    const auto m = fenceRe().match(line);
    if (!m.hasMatch()) return QChar();
    return m.captured(1).at(0);
}

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

EditResult setStatus(const QString &content, const QString &newStatus) {
    bool ewn = false;
    QStringList lines = toLines(content, ewn);
    static const QRegularExpression statusRe(
        QStringLiteral("^\\*\\*Status:\\*\\*"));
    for (int i = 0; i < lines.size(); ++i) {
        if (statusRe.match(lines.at(i)).hasMatch()) {
            lines[i] = QStringLiteral("**Status:** ") + newStatus;
            EditResult r;
            r.ok = true;
            r.content = lines.join(QLatin1Char('\n'));
            if (ewn) r.content += QLatin1Char('\n');
            // 1-based line of the rewritten Status line.
            r.line = i + 1;  // 1-based line of the rewritten Status line
            return r;
        }
    }
    return fail(QStringLiteral("unrecognised_format"),
                QStringLiteral("spec_log: no \"**Status:**\" line found "
                               "(set_status needs one to rewrite)"));
}

EditResult appendLoop(const QString &content, const QString &label,
                      const QString &body) {
    bool ewn = false;
    QStringList lines = toLines(content, ewn);
    const QString bullet = QStringLiteral("- **") + label +
                           QStringLiteral("** — ") + body;
    const int hdr = findSectionHeading(
        lines, QStringLiteral("Cold-eyes loop log"));
    if (hdr >= 0) {
        const auto ins = insertAtSectionEnd(lines, hdr, bullet, ewn);
        EditResult r;
        r.ok = true;
        r.content = ins.content;
        r.line = ins.line;
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
