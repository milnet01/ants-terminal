// ANTS-1961 / ANTS-1962 — implementation of the shared feedback-file
// module. See feedbackfile.h + docs/standards/mcp-feedback-files.md.

#include "feedbackfile.h"

#include <QRegularExpression>
#include <QSet>

namespace FeedbackFile {

namespace {

// Anchor regex for a maintainer tracking-block heading. Canonical home:
// mcp-feedback-files.md § "Maintainer tracking block". The 📋 and the
// literal phrase are mandatory; the optional " update" and everything
// inside the parens are freeform.
const QRegularExpression &maintainerAnchorRe() {
    // The 📋 (U+1F4CB) must be matched as a real UTF-8 code point;
    // build the pattern via fromUtf8 so the emoji bytes are decoded
    // rather than treated as separate Latin-1 chars.
    static const QRegularExpression re(QString::fromUtf8(
        "^## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking"
        "( update)? \\("));
    return re;
}

// Fence opener: ``` or ~~~ with up to 3 leading spaces (CommonMark).
// Captures the fence char so the closer can match the same character.
const QRegularExpression &fenceRe() {
    static const QRegularExpression re(
        QStringLiteral("^\\s{0,3}(```|~~~)"));
    return re;
}

// A boundary heading is exactly one or two leading hashes followed by a
// space (`###`+ are inert body lines).
bool isBoundaryHeading(const QString &line) {
    if (line.startsWith(QStringLiteral("## ")) &&
        !line.startsWith(QStringLiteral("### "))) {
        return true;
    }
    if (line.startsWith(QStringLiteral("# "))) return true;
    return false;
}

bool isMaintainerHeading(const QString &line) {
    return maintainerAnchorRe().match(line).hasMatch();
}

// Returns the fence-char that closes an open fence on `line`, or a null
// QChar if the line is not a fence opener.
QChar fenceOpenerChar(const QString &line) {
    const auto m = fenceRe().match(line);
    if (!m.hasMatch()) return QChar();
    return m.captured(1).at(0);
}

}  // namespace

ParseResult parse(const QString &fileContent) {
    ParseResult r;

    // Split keeping empty trailing segments so 1-based line numbers map
    // cleanly. QString::split with KeepEmptyParts gives one entry per
    // line; a file ending in "\n" yields a trailing empty entry, which
    // is harmless (it is never a heading).
    const QStringList lines =
        fileContent.split(QLatin1Char('\n'));

    // Pass 1 — classify every non-fenced boundary heading. Track the
    // greatest-position maintainer heading and, separately, the index of
    // the first contributor heading after it (the delta start).
    QChar openFence;  // null when not inside a fence
    int lastMaintainerIdx = -1;          // 0-based line index
    // For each line index that is a maintainer heading, remember it so we
    // can find the first contributor heading strictly after the max one.
    QVector<int> maintainerIdx;
    QVector<int> contributorIdx;

    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            // Inside a fence — only the matching closer ends it.
            const QChar c = fenceOpenerChar(line);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(line);
        if (!opener.isNull()) {
            openFence = opener;
            continue;
        }
        if (!isBoundaryHeading(line)) continue;
        if (isMaintainerHeading(line)) {
            maintainerIdx.append(i);
            lastMaintainerIdx = i;
        } else {
            contributorIdx.append(i);
        }
    }

    r.maintainerBlockCount = maintainerIdx.size();
    r.lastMaintainerLine =
        (lastMaintainerIdx >= 0) ? (lastMaintainerIdx + 1) : -1;

    // Delta start: the first contributor heading strictly after the last
    // maintainer heading. When there is no maintainer heading, the delta
    // is everything after the FIRST boundary heading (the H1 title) —
    // i.e. the second boundary heading onward, or, if there is only the
    // title, the line after the title.
    int deltaStartIdx = -1;  // 0-based line index of the delta's first line
    if (lastMaintainerIdx >= 0) {
        for (int idx : contributorIdx) {
            if (idx > lastMaintainerIdx) { deltaStartIdx = idx; break; }
        }
    } else {
        // Zero maintainer blocks → delta = everything after the H1 title
        // (the first boundary heading) to EOF.
        if (!contributorIdx.isEmpty()) {
            // First boundary heading is the title; the delta starts on
            // the line AFTER it.
            deltaStartIdx = contributorIdx.first() + 1;
        }
        // No boundary heading at all → no delta.
    }

    // Pass 2 — mapped IDs: ANTS-[0-9]+ found within maintainer-block
    // bodies only. A maintainer block runs from its heading to the next
    // contributor heading after it (or EOF for the last one). Fenced
    // regions inside the body are still scanned for IDs — the fence rule
    // only governs boundary detection, not ID extraction; an ID pasted
    // inside a maintainer-block fence is still a mapped ID.
    static const QRegularExpression idRe(QStringLiteral("ANTS-[0-9]+"));
    QSet<QString> idSet;
    for (int mi = 0; mi < maintainerIdx.size(); ++mi) {
        const int start = maintainerIdx.at(mi);
        // Block end = the first boundary heading of EITHER kind after
        // `start` (or EOF) — a consecutive maintainer block or the next
        // contributor heading both terminate this block's body.
        int end = lines.size();
        for (int ci : contributorIdx)
            if (ci > start && ci < end) { end = ci; break; }
        if (mi + 1 < maintainerIdx.size() &&
            maintainerIdx.at(mi + 1) < end)
            end = maintainerIdx.at(mi + 1);
        for (int li = start; li < end; ++li) {
            auto it = idRe.globalMatch(lines.at(li));
            while (it.hasNext()) idSet.insert(it.next().captured(0));
        }
    }
    r.mappedIds = QStringList(idSet.begin(), idSet.end());
    r.mappedIds.sort();

    // Build the delta text + line count.
    if (deltaStartIdx >= 0 && deltaStartIdx < lines.size()) {
        // Trim leading blank lines so a zero-maintainer file whose title
        // is followed by blank lines still reports the delta as starting
        // at the first real content line (the H1-title case).
        int s = deltaStartIdx;
        // For the zero-maintainer case the delta starts after the title;
        // skip leading blank lines to land on the first content line.
        if (lastMaintainerIdx < 0) {
            while (s < lines.size() && lines.at(s).trimmed().isEmpty()) ++s;
        }
        if (s < lines.size()) {
            QStringList deltaLines;
            for (int li = s; li < lines.size(); ++li)
                deltaLines.append(lines.at(li));
            // Drop a single trailing empty segment produced by a file
            // that ends in "\n" so the line count matches the visible
            // line count.
            if (!deltaLines.isEmpty() && deltaLines.last().isEmpty())
                deltaLines.removeLast();
            if (!deltaLines.isEmpty()) {
                r.delta = deltaLines.join(QLatin1Char('\n'));
                r.deltaPresent = true;
                r.deltaStartLine = s + 1;  // 1-based
                r.deltaLineCount = deltaLines.size();
            }
        }
    }

    return r;
}

// ---------------------------------------------------------------------

QString renderFindingBlock(const QString &date, const QString &sessionLabel,
                           bool h1Heading, const QString &note,
                           const QVector<Finding> &findings) {
    QString out;
    const QString hashes = h1Heading ? QStringLiteral("# ")
                                     : QStringLiteral("## ");
    out += hashes + date;
    if (!sessionLabel.isEmpty())
        out += QStringLiteral(" — ") + sessionLabel;
    out += QStringLiteral("\n");

    if (!note.isEmpty())
        out += QStringLiteral("\n") + note + QStringLiteral("\n");

    for (const Finding &f : findings) {
        out += QStringLiteral("\n### ") + f.title + QStringLiteral("\n\n");
        if (!f.what.isEmpty())
            out += QStringLiteral("- **What:** ") + f.what +
                   QStringLiteral("\n");
        if (!f.repro.isEmpty())
            out += QStringLiteral("- **Repro:** ") + f.repro +
                   QStringLiteral("\n");
        if (!f.impact.isEmpty())
            out += QStringLiteral("- **Impact:** ") + f.impact +
                   QStringLiteral("\n");
        if (!f.suggestedFix.isEmpty())
            out += QStringLiteral("- **Suggested fix:** ") + f.suggestedFix +
                   QStringLiteral("\n");
        // Always emitted blank — the contributor never assigns an ID
        // (ANTS-1962 INV-6).
        out += QStringLiteral("- **Proposed ID:** _(maintainer to assign)_\n");
    }
    return out;
}

QString renderTrackingBlock(const QString &date, const QString &note,
                            const QVector<TrackingRow> &rows, bool sentinel) {
    QString out;
    out += QString::fromUtf8(
               "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking update (") +
           date + QStringLiteral(", maintainer)\n");
    if (!note.isEmpty())
        out += QStringLiteral("\n") + note + QStringLiteral("\n");

    bool anyNotes = false;
    for (const TrackingRow &row : rows)
        if (!row.notes.isEmpty()) { anyNotes = true; break; }

    out += QStringLiteral("\n");
    if (anyNotes) {
        out += QStringLiteral("| Item | ID(s) | Status | Notes |\n");
        out += QStringLiteral("|------|-------|--------|-------|\n");
    } else {
        out += QStringLiteral("| Item | ID(s) | Status |\n");
        out += QStringLiteral("|------|-------|--------|\n");
    }
    for (const TrackingRow &row : rows) {
        const QString idCell =
            row.ids.isEmpty() ? QStringLiteral("n/a")
                              : row.ids.join(QStringLiteral(", "));
        out += QStringLiteral("| ") + row.item + QStringLiteral(" | ") +
               idCell + QStringLiteral(" | ") + row.status;
        if (anyNotes)
            out += QStringLiteral(" | ") + row.notes;
        out += QStringLiteral(" |\n");
    }
    if (sentinel)
        out += QStringLiteral("\nEnd of ") + date +
               QStringLiteral(" maintainer roadmap-tracking update.\n");
    return out;
}

QString skeleton(const QString &projectTitle) {
    QString out;
    out += QStringLiteral("<!-- ants-mcp-feedback: 1 -->\n");
    out += QStringLiteral("# Ants MCP Feedback — ") + projectTitle +
           QStringLiteral("\n\n");
    out += QStringLiteral(
        "> Format: docs/standards/mcp-feedback-files.md in the Ants "
        "Terminal repo.\n"
        "> **Contributors (ANTS-2226):** read new items with the "
        "`feedback_query` Ants-MCP\n"
        "> verb (the un-triaged tail) and append findings with `feedback_log "
        "op:append_finding`\n"
        "> — don't hand-edit, that keeps the read-the-tail watermark intact. "
        "The maintainer\n"
        "> stamps roadmap IDs via `feedback_log op:append_tracking`.\n"
        "> Contributors append below the last maintainer block; never "
        "edit a\n"
        "> maintainer table; never assign ANTS-NNNN IDs.\n");
    return out;
}

}  // namespace FeedbackFile
