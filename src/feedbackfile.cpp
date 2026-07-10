// ANTS-1961 / ANTS-1962 — implementation of the shared feedback-file
// module. See feedbackfile.h + docs/standards/mcp-feedback-files.md.

#include "feedbackfile.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

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

// ANTS-3421 — single-source fence-aware boundary scan. One `#`/`## `
// heading outside a fenced region is a boundary; `###`+ are inert body
// lines. Both parse() (below) and compactShipped() call this so the
// parser contract has exactly one implementation (CLAUDE.md §3). Returns
// one Boundary per heading in document order.
struct Boundary {
    int     line0;         // 0-based line index of the heading
    QString text;          // the heading line, verbatim
    bool    isMaintainer;  // matches the maintainer anchor regex
};

QVector<Boundary> scanBoundaries(const QStringList &lines) {
    QVector<Boundary> out;
    QChar openFence;  // null when not inside a fence
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            // Inside a fence — only the matching closer ends it.
            const QChar c = fenceOpenerChar(line);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(line);
        if (!opener.isNull()) { openFence = opener; continue; }
        if (!isBoundaryHeading(line)) continue;
        out.append({i, line, isMaintainerHeading(line)});
    }
    return out;
}

// ANTS-3448 — version marker regex, digit-optional (`[0-9]*`) so a malformed
// `<!-- ants-mcp-feedback: -->` captures "" (→ version 0), not a parse error.
// Shared by markerVersion() (read side) and migrateV2()'s presence/line scan
// (write side) so both sides recognise the marker identically (INV-12).
const QRegularExpression &markerRe() {
    static const QRegularExpression re(
        QStringLiteral("<!--\\s*ants-mcp-feedback:\\s*([0-9]*)\\s*-->"));
    return re;
}

// ANTS-3448 — closure regex: a `**Proposed ID:**` value beginning the literal
// `n/a` followed by a word boundary (whitespace, dash, em-dash, or EOL) — so
// `n/architecture` is NOT a closure. Hoisted from compactResolved's
// function-local static so compactResolved() and parse()'s v2 branch share one
// definition and cannot drift on the closure classification (spec § 2.2 / §7).
const QRegularExpression &closureRe() {
    static const QRegularExpression re(
        QStringLiteral("^n/a\\b"), QRegularExpression::CaseInsensitiveOption);
    return re;
}

// ANTS-3448 — finding-bullet arm: a `- **What/Repro/Impact:**` (or `**What**:`)
// body bullet. Hoisted from migrateV2's function-local static so migrateV2()'s
// finding-shaped discriminator and parse()'s suspected_untagged scan share one
// definition (spec § 2.2 step 5 / §7). Narrower than migrate_v2's full
// discriminator, which also has a heading arm.
const QRegularExpression &bulletArmRe() {
    static const QRegularExpression re(
        QStringLiteral("^ *[-*] +\\*\\*(what|repro|impact)[:*]"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

}  // namespace

int markerVersion(const QString &content) {
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const auto m = markerRe().match(line);
        if (m.hasMatch())
            return m.captured(1).isEmpty() ? 0 : m.captured(1).toInt();
    }
    return 0;  // absent or malformed (no digits) ⟹ 0 (< 2, the v1 rule)
}

ParseResult parse(const QString &fileContent) {
    ParseResult r;

    // Split keeping empty trailing segments so 1-based line numbers map
    // cleanly. QString::split with KeepEmptyParts gives one entry per
    // line; a file ending in "\n" yields a trailing empty entry, which
    // is harmless (it is never a heading).
    const QStringList lines =
        fileContent.split(QLatin1Char('\n'));

    // ANTS-3448 — format version from the marker. >= 2 selects the v2
    // inline-`**Proposed ID:**` delta rule (§2.2); < 2 keeps the v1 rule. The
    // v1 boundary scan below runs on BOTH versions (INV-6) — only the delta /
    // mappedIds / suspectedUntagged fields branch on the version.
    r.formatVersion = markerVersion(fileContent);
    const bool v2 = r.formatVersion >= 2;

    // Pass 1 — classify every non-fenced boundary heading via the shared
    // scanner (ANTS-3421 extraction). Track the greatest-position
    // maintainer heading and, separately, the index of the first
    // contributor heading after it (the delta start).
    int lastMaintainerIdx = -1;          // 0-based line index
    QVector<int> maintainerIdx;
    QVector<int> contributorIdx;
    for (const Boundary &b : scanBoundaries(lines)) {
        if (b.isMaintainer) {
            maintainerIdx.append(b.line0);
            lastMaintainerIdx = b.line0;
        } else {
            contributorIdx.append(b.line0);
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
    // ANTS-3445 — anchored whole-token id test for the tracking-row ID column
    // (a cell holds only a real id, never an id embedded in prose).
    static const QRegularExpression idCellRe(QStringLiteral("^ANTS-[0-9]+$"));
    // ANTS-3371 — separator row of a GFM table (`|---|:--:|` etc.).
    static const QRegularExpression sepCellRe(QStringLiteral("^:?-{1,}:?$"));
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
            const QString &row = lines.at(li);
            auto it = idRe.globalMatch(row);
            while (it.hasNext()) idSet.insert(it.next().captured(0));

            // ANTS-3371 — extract the maintainer tracking-table data rows
            // so include_tracking can return per-item status without a
            // full-file Read + hand-parse. A pipe-table row trims to a
            // leading `|`; split on `|`, drop the empty leading/trailing
            // cells, skip the header (`Item …`) and `|---|` separator.
            const QString trimmed = row.trimmed();
            if (!trimmed.startsWith(QLatin1Char('|'))) continue;
            QStringList cells = trimmed.split(QLatin1Char('|'));
            if (!cells.isEmpty() && cells.first().trimmed().isEmpty())
                cells.removeFirst();
            if (!cells.isEmpty() && cells.last().trimmed().isEmpty())
                cells.removeLast();
            for (QString &c : cells) c = c.trimmed();
            if (cells.size() < 3) continue;
            if (cells.at(0).compare(QStringLiteral("Item"),
                                    Qt::CaseInsensitive) == 0)
                continue;  // header row
            if (sepCellRe.match(cells.at(0)).hasMatch())
                continue;  // |---| separator row
            TrackingRow tr;
            tr.line = li + 1;   // ANTS-3442: 1-based source line
            tr.item = cells.at(0);
            const QString idCell = cells.at(1);
            // ANTS-3445 — only a real ANTS-NNNN token is an id. A non-id
            // id-column value (a closure like `(schema fix)` / `(self-resolved)`
            // / `n/a`, or a bare local counter) must NOT populate tr.ids, or
            // pruneTracking treats the shared pseudo-id as "superseded" and drops
            // the earlier of two unrelated closure rows (confirmed data loss on
            // MAME_Curator #4). The explicit `n/a` skip is now subsumed — it
            // simply fails the id-shape test, like every other non-id token.
            for (const QString &p : idCell.split(QLatin1Char(','))) {
                const QString id = p.trimmed();
                if (idCellRe.match(id).hasMatch()) tr.ids.append(id);
            }
            tr.status = cells.at(2);
            if (cells.size() >= 4) tr.notes = cells.at(3);
            r.trackingRows.append(tr);
        }
    }
    r.mappedIds = QStringList(idSet.begin(), idSet.end());
    r.mappedIds.sort();

    // ANTS-3448 — the delta / mappedIds / suspectedUntagged fields branch on
    // the format version. The v1 boundary scan above (lastMaintainerLine /
    // maintainerBlockCount / trackingRows) already ran on both versions (INV-6).
    if (v2) {
        // --- v2 rule (spec § 2.2): the un-triaged delta is the ordered
        // concatenation of every `### ` finding whose first `**Proposed ID:**`
        // value is neither an ANTS-NNNN id nor an `n/a` closure. ------------
        const QVector<FindingBlock> blocks = enumerateFindingBlocks(lines);

        // Fence-aware body scan: does this line-less block carry a finding
        // bullet (`- **What/Repro/Impact:**`)? → suspected_untagged (§2.2/5).
        auto bodyHasFindingBullet = [&](const FindingBlock &fb) -> bool {
            QChar bodyFence;
            for (int li = fb.headingLine0 + 1;
                 li < fb.extentEnd0 && li < lines.size(); ++li) {
                const QString &bl = lines.at(li);
                if (!bodyFence.isNull()) {
                    const QChar c = fenceOpenerChar(bl);
                    if (!c.isNull() && c == bodyFence) bodyFence = QChar();
                    continue;
                }
                const QChar opener = fenceOpenerChar(bl);
                if (!opener.isNull()) { bodyFence = opener; continue; }
                if (bulletArmRe().match(bl).hasMatch()) return true;
            }
            return false;
        };

        QSet<QString> v2ids;
        QStringList deltaBlocks;     // one `\n`-joined block per un-triaged finding
        int deltaLineTotal = 0;
        for (const FindingBlock &fb : blocks) {
            if (fb.idLine0 < 0) {
                // No id line → not a finding. A finding-shaped one is a
                // hand-editor silent-loss risk (suspected_untagged); bare prose
                // is ignored.
                if (bodyHasFindingBullet(fb))
                    r.suspectedUntagged.append({ fb.heading, fb.headingLine0 + 1 });
                continue;
            }
            const QString val = fb.idValue.trimmed();
            const bool isClosure = closureRe().match(val).hasMatch();
            const bool hasId = idRe.match(val).hasMatch();
            if (!isClosure && hasId) {
                // Triaged with real id(s): contribute them to mappedIds.
                auto it = idRe.globalMatch(val);
                while (it.hasNext()) v2ids.insert(it.next().captured(0));
                continue;   // triaged → not in the delta
            }
            if (isClosure) continue;  // closure → triaged, contributes no ids
            // Un-triaged (empty / placeholder / any non-id, non-n/a text): the
            // finding's block, all trailing blank lines stripped per block.
            int end0 = fb.extentEnd0;
            while (end0 > fb.headingLine0 + 1 &&
                   lines.at(end0 - 1).trimmed().isEmpty())
                --end0;
            QStringList blk;
            for (int li = fb.headingLine0; li < end0 && li < lines.size(); ++li)
                blk.append(lines.at(li));
            if (r.deltaStartLine < 0) r.deltaStartLine = fb.headingLine0 + 1;
            deltaLineTotal += blk.size();
            deltaBlocks.append(blk.join(QLatin1Char('\n')));
        }
        r.mappedIds = QStringList(v2ids.begin(), v2ids.end());
        r.mappedIds.sort();
        if (!deltaBlocks.isEmpty()) {
            r.delta = deltaBlocks.join(QLatin1Char('\n'));
            r.deltaPresent = true;
            r.deltaLineCount = deltaLineTotal;
        }
        return r;
    }

    // --- v1 rule (formatVersion < 2): the current boundary-watermark delta ---
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

// ANTS-3469 — a GFM table cell must not carry a raw `|` (splits the column,
// mis-aligning Status/Notes) or a newline (breaks the row). Escape both so an
// item/notes string containing a pipe (e.g. a regex `a|b`) round-trips with
// the right column count. Mirrors roadmap_log op:bundle_row's escapeCell
// (remotecontrol.cpp, ANTS-1691); IDs/status carry no pipes but escaping them
// uniformly is harmless and keeps the renderer trivially correct.
static QString escapeTrackingCell(const QString &raw) {
    QString s = raw;
    s.replace(QStringLiteral("|"), QStringLiteral("\\|"));
    s.replace(QStringLiteral("\r\n"), QStringLiteral("<br>"));
    s.replace(QChar('\n'), QStringLiteral("<br>"));
    s.replace(QChar('\r'), QStringLiteral("<br>"));
    return s.trimmed();
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
        out += QStringLiteral("| ") + escapeTrackingCell(row.item) +
               QStringLiteral(" | ") + escapeTrackingCell(idCell) +
               QStringLiteral(" | ") + escapeTrackingCell(row.status);
        if (anyNotes)
            out += QStringLiteral(" | ") + escapeTrackingCell(row.notes);
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

// ANTS-3421 — collapse confirmed-shipped contributor blocks to a one-line
// stub. Pure; the wrapper (cmdFeedbackLog) handles path + request-shape
// validation and the atomic write. Gates + invariants: docs/specs/ANTS-3421.md.
CompactResult compactShipped(const QString &content,
                             const QVector<CompactTarget> &targets) {
    CompactResult res;
    const QStringList lines = content.split(QLatin1Char('\n'));
    const QVector<Boundary> boundaries = scanBoundaries(lines);
    const ParseResult pr = parse(content);

    // "→ shipped ANTS-" idempotency sentinel (gate 7) and the ✅ leading
    // token (gate 5).
    static const QString kStubPrefix =
        QString::fromUtf8("\xE2\x86\x92 shipped ANTS-");
    static const QChar   kCheckMark = QChar(0x2705);  // ✅

    // Block end (0-based, exclusive) = next boundary of EITHER level, or EOF
    // — byte-identical to parse()'s block-end rule.
    auto blockEndFor = [&](int bi) -> int {
        return (bi + 1 < boundaries.size()) ? boundaries.at(bi + 1).line0
                                            : lines.size();
    };
    // Fence-aware count of `### ` finding sub-headings in [start,end) (gate 6).
    auto findingHeadingCount = [&](int start, int end) -> int {
        int n = 0;
        QChar openFence;
        for (int li = start; li < end && li < lines.size(); ++li) {
            const QString &l = lines.at(li);
            if (!openFence.isNull()) {
                const QChar c = fenceOpenerChar(l);
                if (!c.isNull() && c == openFence) openFence = QChar();
                continue;
            }
            const QChar opener = fenceOpenerChar(l);
            if (!opener.isNull()) { openFence = opener; continue; }
            if (l.startsWith(QStringLiteral("### "))) ++n;
        }
        return n;
    };
    // First non-blank body line in [start,end) (gate 7).
    auto firstNonBlank = [&](int start, int end) -> QString {
        for (int li = start; li < end && li < lines.size(); ++li)
            if (!lines.at(li).trimmed().isEmpty()) return lines.at(li);
        return QString();
    };
    // Whole-block byte size over [start0,end0) of a line list.
    auto blockBytes = [](const QStringList &src, int start0, int end0) -> int {
        QStringList seg;
        for (int li = start0; li < end0 && li < src.size(); ++li)
            seg.append(src.at(li));
        return seg.join(QLatin1Char('\n')).toUtf8().size();
    };

    res.results.resize(targets.size());
    QVector<int> resolvedBi(targets.size(), -1);  // gate-1 resolved boundary idx

    // Phase A — gate 1: resolve each target's heading to one boundary.
    for (int t = 0; t < targets.size(); ++t) {
        const CompactTarget &tg = targets.at(t);
        CompactOutcome &o = res.results[t];
        o.heading = tg.heading;
        o.id      = tg.id;
        const QString want = tg.heading.trimmed();
        if (tg.headingLine > 0) {
            const int wantLine0 = tg.headingLine - 1;
            int bi = -1;
            for (int k = 0; k < boundaries.size(); ++k)
                if (boundaries.at(k).line0 == wantLine0) { bi = k; break; }
            if (bi < 0) {
                o.code = QStringLiteral("target_not_found");
                o.reason = QStringLiteral("heading_line %1 is not a boundary "
                                          "heading").arg(tg.headingLine);
                continue;
            }
            if (!want.isEmpty() && boundaries.at(bi).text.trimmed() != want) {
                o.code = QStringLiteral("target_not_found");
                o.reason = QStringLiteral("heading disagrees with heading_line "
                                          "%1").arg(tg.headingLine);
                continue;
            }
            resolvedBi[t] = bi;
        } else {
            QVector<int> matches;
            for (int bi = 0; bi < boundaries.size(); ++bi)
                if (boundaries.at(bi).text.trimmed() == want) matches.append(bi);
            if (matches.isEmpty()) {
                o.code = QStringLiteral("target_not_found");
                o.reason = QStringLiteral("no boundary heading matches");
                continue;
            }
            if (matches.size() > 1) {
                o.code = QStringLiteral("target_ambiguous");
                o.reason = QStringLiteral("%1 headings match; pass heading_line")
                               .arg(matches.size());
                for (int bi : matches) o.candidates.append(boundaries.at(bi).line0 + 1);
                continue;
            }
            resolvedBi[t] = matches.first();
        }
    }

    // Phase B — cross-target duplicate: two targets on the same block are
    // both refused duplicate_target (precedes gates 2–7, follows gate 1).
    QHash<int, int> biCount;
    for (int t = 0; t < targets.size(); ++t)
        if (resolvedBi[t] >= 0) biCount[resolvedBi[t]]++;
    for (int t = 0; t < targets.size(); ++t) {
        if (resolvedBi[t] >= 0 && biCount.value(resolvedBi[t]) > 1) {
            res.results[t].code = QStringLiteral("duplicate_target");
            res.results[t].reason =
                QStringLiteral("another target names the same block");
        }
    }

    // Phase C — gates 2–7 (top-to-bottom, first failure wins) for the
    // targets that resolved and are not duplicate-collisions.
    for (int t = 0; t < targets.size(); ++t) {
        if (resolvedBi[t] < 0 || !res.results[t].code.isEmpty()) continue;
        const int bi = resolvedBi[t];
        const Boundary &b = boundaries.at(bi);
        CompactOutcome &o = res.results[t];
        const int end = blockEndFor(bi);       // 0-based exclusive
        const int hdrLine1 = b.line0 + 1;       // 1-based heading line

        if (bi == 0) {                          // gate 2
            o.code = QStringLiteral("title_block");
            o.reason = QStringLiteral("the H1 title / contributor banner block");
            continue;
        }
        if (b.isMaintainer) {                   // gate 3
            o.code = QStringLiteral("maintainer_block");
            o.reason = QStringLiteral("a maintainer tracking watermark");
            continue;
        }
        if (pr.lastMaintainerLine < 0) {        // gate 4 (no watermark)
            o.code = QStringLiteral("not_triaged");
            o.reason = QStringLiteral("file has zero maintainer blocks");
            continue;
        }
        if (hdrLine1 >= pr.lastMaintainerLine) {  // gate 4 (below watermark)
            o.code = QStringLiteral("in_delta");
            o.reason = QStringLiteral("heading is below the watermark");
            continue;
        }
        // gate 5 — effective (last document-order) tracking row for `id`
        // must have a status cell beginning with ✅.
        QString effStatus;
        bool haveRow = false;
        for (const TrackingRow &tr : pr.trackingRows)
            if (tr.ids.contains(o.id)) { effStatus = tr.status; haveRow = true; }
        if (!haveRow) {
            o.code = QStringLiteral("not_shipped");
            o.reason = QStringLiteral("no tracking row");
            continue;
        }
        if (!effStatus.trimmed().startsWith(kCheckMark)) {
            o.code = QStringLiteral("not_shipped");
            o.reason = QStringLiteral("effective status ") + effStatus.trimmed();
            continue;
        }
        const int fh = findingHeadingCount(b.line0 + 1, end);  // gate 6
        if (fh >= 2) {
            o.code = QStringLiteral("multi_finding");
            o.reason = QStringLiteral("%1 finding sub-headings").arg(fh);
            continue;
        }
        const QString fnb = firstNonBlank(b.line0 + 1, end);   // gate 7
        if (fnb.trimmed().startsWith(kStubPrefix)) {
            o.code = QStringLiteral("already_compacted");
            o.reason = QStringLiteral("already a compacted stub");
            continue;
        }

        o.applied     = true;
        o.startLine   = hdrLine1;
        o.endLine     = end;  // 1-based last body line (0-based end-1 → +1)
        o.bytesBefore = blockBytes(lines, b.line0, end);
    }

    // Apply bottom-up so an earlier collapse never shifts a later target's
    // resolved line positions.
    QStringList outLines = lines;
    QVector<int> appliedT;
    for (int t = 0; t < targets.size(); ++t)
        if (res.results[t].applied) appliedT.append(t);
    std::sort(appliedT.begin(), appliedT.end(), [&](int a, int c) {
        return boundaries.at(resolvedBi[a]).line0 >
               boundaries.at(resolvedBi[c]).line0;
    });
    for (int t : appliedT) {
        const CompactTarget &tg = targets.at(t);
        CompactOutcome &o = res.results[t];
        const int bi = resolvedBi[t];
        const int start0 = boundaries.at(bi).line0 + 1;  // first body line
        const int end0   = blockEndFor(bi);              // exclusive
        const QString breadcrumb =
            QString::fromUtf8("\xE2\x86\x92 shipped ") + tg.id +
            QStringLiteral(", confirmed ") + tg.session + QLatin1Char(' ') +
            tg.date + QStringLiteral(" (write-up compacted, ANTS-3421)");
        // Uniform replacement body: a blank line, the breadcrumb, a trailing
        // blank — one blank before the next boundary, or a single trailing
        // newline when this is the file's last block.
        const QStringList newBody = { QString(), breadcrumb, QString() };
        for (int k = end0 - 1; k >= start0; --k) outLines.removeAt(k);
        for (int k = newBody.size() - 1; k >= 0; --k)
            outLines.insert(start0, newBody.at(k));
        // bytes_after over the new whole block [heading .. trailing blank].
        QStringList afterBlock = { boundaries.at(bi).text };
        afterBlock += newBody;
        o.bytesAfter = afterBlock.join(QLatin1Char('\n')).toUtf8().size();
        res.bytesSaved += (o.bytesBefore - o.bytesAfter);
    }
    res.newContent = outLines.join(QLatin1Char('\n'));
    return res;
}

// ANTS-3442 — prune_tracking. Remove superseded duplicate maintainer
// tracking rows, keeping the authoritative last-per-id row. Two-stage pass
// (docs/specs/ANTS-3442.md § 2.3): Stage 1 marks id-column-superseded rows;
// Stage 2 removes a marked row only when every ANTS-NNNN token anywhere in
// its line still appears in a surviving line, so parse().mappedIds is
// preserved (INV-2b). Pure; single bottom-up rewrite.
PruneResult pruneTracking(const QString &content, const PruneOptions &opts) {
    PruneResult res;
    const QStringList lines = content.split(QLatin1Char('\n'));
    const ParseResult pr = parse(content);
    static const QRegularExpression idRe(QStringLiteral("ANTS-[0-9]+"));

    const QSet<QString> scope(opts.scopeIds.begin(), opts.scopeIds.end());

    // Maintainer-block line indices (0-based): every line from a maintainer
    // heading to the next boundary of either level. parse() builds mappedIds
    // from exactly these lines, so the surviving-token accounting matches.
    const QVector<Boundary> boundaries = scanBoundaries(lines);
    QSet<int> maintainerLines;
    for (int bi = 0; bi < boundaries.size(); ++bi) {
        if (!boundaries.at(bi).isMaintainer) continue;
        const int start = boundaries.at(bi).line0;
        const int end = (bi + 1 < boundaries.size())
                            ? boundaries.at(bi + 1).line0 : lines.size();
        for (int i = start; i < end && i < lines.size(); ++i)
            maintainerLines.insert(i);
    }

    // Stage 1 — mark id-column-superseded rows. lastLineForId[id] = greatest
    // source line among tracking rows citing id; a row is marked iff every one
    // of its ids has a later duplicate (and, when scoped, is in scope).
    QHash<QString, int> lastLineForId;
    for (const TrackingRow &tr : pr.trackingRows)
        for (const QString &id : tr.ids)
            lastLineForId[id] = qMax(lastLineForId.value(id, -1), tr.line);

    QSet<int> markedLines;                 // 1-based lines of marked data rows
    QVector<const TrackingRow *> marked;   // document order
    for (const TrackingRow &tr : pr.trackingRows) {
        if (tr.ids.isEmpty()) continue;    // clause 1 — never mark an n/a row
        bool superseded = true;
        for (const QString &id : tr.ids)
            if (lastLineForId.value(id, -1) <= tr.line) { superseded = false; break; }
        if (!superseded) continue;         // clause 2 — last-of-any-id survives
        if (!scope.isEmpty()) {            // clause 3 — ID-column scope
            bool allIn = true;
            for (const QString &id : tr.ids)
                if (!scope.contains(id)) { allIn = false; break; }
            if (!allIn) continue;
        }
        markedLines.insert(tr.line);
        marked.append(&tr);
    }

    // Stage 2 — surviving-token set = ANTS-NNNN over maintainer-block lines
    // that are NOT marked data-row lines (fixed before any removal). A marked
    // row is removed iff every token in its line still appears in a surviving
    // line; otherwise it is pinned (its notes/prose id lives only on marked
    // rows).
    QSet<QString> survivingTokens;
    for (int i : maintainerLines) {
        if (markedLines.contains(i + 1)) continue;
        auto it = idRe.globalMatch(lines.at(i));
        while (it.hasNext()) survivingTokens.insert(it.next().captured(0));
    }

    QVector<int> removeLines;   // 1-based
    for (const TrackingRow *tr : marked) {
        const QString &rowLine = lines.at(tr->line - 1);
        bool safe = true;
        auto it = idRe.globalMatch(rowLine);
        while (it.hasNext())
            if (!survivingTokens.contains(it.next().captured(0))) { safe = false; break; }
        if (!safe) continue;    // pinned — keep
        removeLines.append(tr->line);
        PrunedRow row;
        row.ids    = tr->ids;
        row.status = tr->status;
        row.line   = tr->line;
        res.removed.append(row);   // document order (marked is in order)
    }

    // Rewrite — remove chosen lines bottom-up so earlier removals don't shift
    // not-yet-removed indices. Headings / header / separator are never in
    // removeLines (parse() never emits them as tracking rows), so INV-5 holds.
    QStringList out = lines;
    std::sort(removeLines.begin(), removeLines.end(), std::greater<int>());
    for (int ln : removeLines)
        if (ln >= 1 && ln <= out.size()) out.removeAt(ln - 1);
    res.newContent = out.join(QLatin1Char('\n'));
    res.bytesSaved = static_cast<long>(content.toUtf8().size())
                   - static_cast<long>(res.newContent.toUtf8().size());
    return res;
}

// ANTS-3443 — fence-aware `### `-block enumerator. Shared scanner for the
// v2 consumers (compact_resolved here; feedback_query delta + migrate_v2
// pending). A block runs from its `### ` heading to the next
// `#`/`## `/`### ` boundary (or EOF), fences skipped; idLine0/idValue carry
// the block's FIRST `**Proposed ID:**` line (the standard's canonical
// id-line regex, first-line-wins). Classification (finding vs prose) is the
// caller's, not the scanner's.
QVector<FindingBlock> enumerateFindingBlocks(const QStringList &lines) {
    // Canonical id-line regex — mcp-feedback-files.md § "Maintainer triage".
    // The `[ *:]+` run swallows the `:**`/`**:` separator in either order.
    static const QRegularExpression idLineRe(
        QStringLiteral("^ *(?:[-*] +)?\\*{0,2} *proposed id[ *:]+(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    // 1–3 hash boundary (`# `/`## `/`### `); a `### ` also terminates a block,
    // unlike the v1 scanBoundaries (which treats `### ` as inert).
    static const QRegularExpression boundaryRe(QStringLiteral("^#{1,3} "));

    // Pass 1 — every boundary heading outside a fence, in document order.
    struct B { int line0; bool isSub; };   // isSub ⟹ a `### ` heading
    QVector<B> bounds;
    QChar openFence;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines.at(i);
        if (!openFence.isNull()) {
            const QChar c = fenceOpenerChar(l);
            if (!c.isNull() && c == openFence) openFence = QChar();
            continue;
        }
        const QChar opener = fenceOpenerChar(l);
        if (!opener.isNull()) { openFence = opener; continue; }
        if (boundaryRe.match(l).hasMatch())
            bounds.append({ i, l.startsWith(QStringLiteral("### ")) });
    }

    // Pass 2 — for each `### ` boundary emit a block; extent = next boundary
    // of any level (or EOF). Every `### ` boundary is guaranteed outside a
    // fence (Pass 1), so the body scan restarts fence tracking cleanly.
    QVector<FindingBlock> out;
    for (int bi = 0; bi < bounds.size(); ++bi) {
        if (!bounds.at(bi).isSub) continue;
        FindingBlock fb;
        fb.headingLine0 = bounds.at(bi).line0;
        fb.heading      = lines.at(fb.headingLine0);
        fb.extentEnd0   = (bi + 1 < bounds.size()) ? bounds.at(bi + 1).line0
                                                   : lines.size();
        QChar bodyFence;
        for (int li = fb.headingLine0 + 1; li < fb.extentEnd0; ++li) {
            const QString &bl = lines.at(li);
            if (!bodyFence.isNull()) {
                const QChar c = fenceOpenerChar(bl);
                if (!c.isNull() && c == bodyFence) bodyFence = QChar();
                continue;
            }
            const QChar opener = fenceOpenerChar(bl);
            if (!opener.isNull()) { bodyFence = opener; continue; }
            const auto m = idLineRe.match(bl);
            if (m.hasMatch()) {
                fb.idLine0 = li;
                QString v = m.captured(1).trimmed();
                while (v.startsWith(QLatin1Char('*'))) v.remove(0, 1);
                while (v.endsWith(QLatin1Char('*')))   v.chop(1);
                fb.idValue = v.trimmed();
                break;   // first matching line wins
            }
        }
        out.append(fb);
    }
    return out;
}

// ANTS-3443 — compact_resolved. Collapse each shipped v2 finding's write-up
// to a `→ shipped ✅ (write-up compacted, ANTS-3443)` stub that retains the
// heading + the first `**Proposed ID:**` line. Gated per-finding on the
// injected roadmap sets (spec § 2.5, first-failure-wins); id-less `### `
// prose blocks are not findings and are neither collapsed nor reported.
// Pure; single bottom-up rewrite.
ResolveResult compactResolved(const QString &content, const ResolveOptions &opts) {
    ResolveResult res;
    const QStringList lines = content.split(QLatin1Char('\n'));
    const QVector<FindingBlock> blocks = enumerateFindingBlocks(lines);

    static const QRegularExpression idTokRe(QStringLiteral("ANTS-[0-9]+"));
    static const QString kShippedProbe = QString::fromUtf8("\xE2\x86\x92 shipped");
    static const QString kBreadcrumb = QString::fromUtf8(
        "\xE2\x86\x92 shipped \xE2\x9C\x85 (write-up compacted, ANTS-3443)");

    auto blockBytes = [&](int start0, int end0) -> int {
        QStringList seg;
        for (int li = start0; li < end0 && li < lines.size(); ++li)
            seg.append(lines.at(li));
        return seg.join(QLatin1Char('\n')).toUtf8().size();
    };
    // gate 2 — any body line (fences skipped) begins "→ shipped" ⟹ already a
    // stub. NB it is "any" body line, not the first: the v2 stub keeps the
    // `**Proposed ID:**` line above the breadcrumb.
    auto hasStub = [&](int start0, int end0) -> bool {
        QChar openFence;
        for (int li = start0; li < end0 && li < lines.size(); ++li) {
            const QString &l = lines.at(li);
            if (!openFence.isNull()) {
                const QChar c = fenceOpenerChar(l);
                if (!c.isNull() && c == openFence) openFence = QChar();
                continue;
            }
            const QChar opener = fenceOpenerChar(l);
            if (!opener.isNull()) { openFence = opener; continue; }
            if (l.trimmed().startsWith(kShippedProbe)) return true;
        }
        return false;
    };

    // Gate each finding (id-less prose blocks omitted entirely).
    struct Plan { int headingLine0, extentEnd0, idLine0, fIdx; };
    QVector<Plan> toCollapse;
    for (const FindingBlock &fb : blocks) {
        if (fb.idLine0 < 0) continue;   // non-finding prose — not a finding
        ResolvedFinding f;
        f.heading = fb.heading;
        f.line    = fb.headingLine0 + 1;
        auto it = idTokRe.globalMatch(fb.idValue);
        while (it.hasNext()) f.ids.append(it.next().captured(0));

        const bool closure = closureRe().match(fb.idValue.trimmed()).hasMatch();
        if (closure || f.ids.isEmpty()) {                       // gate 1
            f.code = QStringLiteral("no_shippable_id");
        } else if (hasStub(fb.headingLine0 + 1, fb.extentEnd0)) { // gate 2
            f.code = QStringLiteral("already_compacted");
        } else {
            QStringList unresolved, open;
            for (const QString &id : f.ids) {
                if (!opts.roadmapIds.contains(id)) unresolved.append(id);
                else if (!opts.shippedIds.contains(id)) open.append(id);
            }
            if (!unresolved.isEmpty()) {                        // gate 3
                f.code = QStringLiteral("roadmap_unresolved_ids");
                f.unresolvedIds = unresolved;
            } else if (!open.isEmpty()) {                       // gate 4
                f.code = QStringLiteral("has_open_id");
                f.openIds = open;
            } else {                                            // all ✅
                f.collapsed   = true;
                f.bytesBefore = blockBytes(fb.headingLine0, fb.extentEnd0);
            }
        }
        const int fIdx = res.findings.size();
        res.findings.append(f);
        if (f.collapsed)
            toCollapse.append({ fb.headingLine0, fb.extentEnd0, fb.idLine0, fIdx });
    }

    // Apply bottom-up so an earlier collapse never shifts a later finding's
    // line positions (parity with compactShipped).
    QStringList outLines = lines;
    std::sort(toCollapse.begin(), toCollapse.end(),
              [](const Plan &a, const Plan &b) {
                  return a.headingLine0 > b.headingLine0;
              });
    for (const Plan &p : toCollapse) {
        ResolvedFinding &f = res.findings[p.fIdx];
        const QString idLineVerbatim = lines.at(p.idLine0);
        const int start0 = p.headingLine0 + 1;   // first body line
        const int end0   = p.extentEnd0;          // exclusive
        // Stub order: heading (untouched) → blank → retained id line →
        // breadcrumb → trailing blank.
        const QStringList newBody = { QString(), idLineVerbatim,
                                      kBreadcrumb, QString() };
        for (int k = end0 - 1; k >= start0; --k) outLines.removeAt(k);
        for (int k = newBody.size() - 1; k >= 0; --k)
            outLines.insert(start0, newBody.at(k));
        QStringList afterBlock = { lines.at(p.headingLine0) };
        afterBlock += newBody;
        f.bytesAfter = afterBlock.join(QLatin1Char('\n')).toUtf8().size();
        res.bytesSaved += (f.bytesBefore - f.bytesAfter);
    }
    res.newContent = outLines.join(QLatin1Char('\n'));
    return res;
}

// ANTS-3446 — migrate_v2. Mechanical v1→v2: bump the version marker to `: 2`
// and stamp a blank `**Proposed ID:**` placeholder on each finding-shaped,
// below-watermark `### ` block that lacks one. Leaves the v1 tracking tables
// in place, so the v1 watermark/delta is preserved (spec § 1.1 / INV-4).
// Pure; single bottom-up apply of the stamp inserts, then the marker bump.
// ANTS-3474 — tokenise a heading / tracking-item for backfill matching:
// lowercase, split on non-[a-z0-9_], keep tokens length ≥ 3, drop stopwords
// and the `ants` / bare-number meta tokens that pollute a maintainer summary
// (e.g. "ANTS-3393", "verified"). Underscores are kept so `roadmap_query` /
// `codebase_index` survive as single distinctive tokens.
static QSet<QString> backfillTokens(const QString &s) {
    static const QSet<QString> stop = {
        QStringLiteral("the"), QStringLiteral("and"), QStringLiteral("for"),
        QStringLiteral("with"), QStringLiteral("not"), QStringLiteral("but"),
        QStringLiteral("its"), QStringLiteral("was"), QStringLiteral("are"),
        QStringLiteral("that"), QStringLiteral("this"), QStringLiteral("when"),
        QStringLiteral("from"), QStringLiteral("into"), QStringLiteral("has"),
        QStringLiteral("had"), QStringLiteral("via"), QStringLiteral("per"),
        QStringLiteral("ants")};
    static const QRegularExpression splitRe(QStringLiteral("[^a-z0-9_]+"));
    static const QRegularExpression numRe(QStringLiteral("^[0-9]+$"));
    QSet<QString> out;
    const QStringList toks =
        s.toLower().split(splitRe, Qt::SkipEmptyParts);
    for (const QString &t : toks) {
        if (t.size() < 3) continue;
        if (stop.contains(t)) continue;
        if (numRe.match(t).hasMatch()) continue;  // bare numbers incl. NNNN
        out.insert(t);
    }
    return out;
}

MigrateResult migrateV2(const QString &content, bool backfillFromTracking) {
    MigrateResult res;
    const QStringList lines = content.split(QLatin1Char('\n'));

    // Version via the shared markerVersion() helper (ANTS-3448 / INV-12) so the
    // read and write sides extract the version identically. migrate_v2 keeps its
    // OWN marker *presence* scan (via the same shared markerRe()) because the
    // helper cannot distinguish present-but-malformed (→ 0) from absent (→ 0),
    // and the insert-vs-repair branch below needs that bit.
    const int ver = markerVersion(content);
    bool markerPresent = false;
    for (const QString &line : lines) {
        if (markerRe().match(line).hasMatch()) { markerPresent = true; break; }
    }
    // Idempotency short-circuit (spec § 2.7 / INV-6): a marker ≥ 2 is left
    // untouched — `>= 2` (not `== 2`) so a future `: 3` file is not downgraded.
    if (ver >= 2) {
        res.alreadyV2 = true;
        res.newContent = content;
        return res;
    }

    // ANTS-3474 — one parse() feeds both the watermark and (opt-in) the
    // tracking rows the backfill matcher reads.
    const ParseResult pr = parse(content);
    const int watermark = pr.lastMaintainerLine;  // 1-based; -1 none
    const QVector<TrackingRow> &trackingRows = pr.trackingRows;
    const QVector<FindingBlock> blocks = enumerateFindingBlocks(lines);

    // ANTS-3474 — match a finding heading to a single tracking-table id.
    // Overlap-coefficient (|H∩R| / min(|H|,|R|)) grouped PER ID so the same id
    // recurring across triage tables can't defeat the uniqueness margin. Returns
    // an empty id when no id clears the confidence floor with a clear margin —
    // ambiguous / folded / paraphrased rows stay blank (precision over recall).
    struct BackfillMatch { QString id; int conf = 0; };
    auto matchId = [&](const QString &heading) -> BackfillMatch {
        const QSet<QString> H = backfillTokens(heading);
        if (H.isEmpty()) return {};
        QHash<QString, double> bestPerId;   // joined-id → best overlap score
        for (const TrackingRow &tr : trackingRows) {
            if (tr.ids.isEmpty()) continue;         // n/a row — no id to carry
            const QSet<QString> R = backfillTokens(tr.item);
            const int denom = qMin(H.size(), R.size());
            if (denom == 0) continue;
            int inter = 0;
            for (const QString &t : R) if (H.contains(t)) ++inter;
            if (inter < 3) continue;                // floor: ≥3 shared tokens
            const double score = double(inter) / denom;
            const QString idKey = tr.ids.join(QStringLiteral(", "));
            bestPerId[idKey] = qMax(bestPerId.value(idKey, 0.0), score);
        }
        QString bestId;
        double best = 0.0, second = 0.0;
        for (auto it = bestPerId.constBegin(); it != bestPerId.constEnd(); ++it) {
            if (it.value() > best) { second = best; best = it.value(); bestId = it.key(); }
            else if (it.value() > second) { second = it.value(); }
        }
        if (best >= 0.66 && (best - second) >= 0.20)
            return { bestId, static_cast<int>(best * 100 + 0.5) };
        return {};
    };

    // §2.4 finding-shaped discriminators. Heading arm (migrate_v2-specific
    // superset) OR body-bullet arm (shared with the standard's suspected-untagged
    // definition; the `[:*]` class matches both `**What:**` and `**What**:`).
    static const QRegularExpression headingArmRe(
        QStringLiteral("\\b(issue|observation)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    // Body-bullet arm shared with parse()'s suspected_untagged scan (ANTS-3448).
    // Byte-identical to renderFindingBlock's placeholder + matches
    // enumerateFindingBlocks's id-line regex (so a re-run detects it as
    // already-stamped — spec § 2.7).
    static const QString kStamp =
        QStringLiteral("- **Proposed ID:** _(maintainer to assign)_");

    // A `### ` block is finding-shaped iff its heading names issue/observation,
    // or its body (fences skipped) carries a `- **What/Repro/Impact:**` bullet.
    auto isFindingShaped = [&](const FindingBlock &fb) -> bool {
        if (headingArmRe.match(fb.heading).hasMatch()) return true;
        QChar bodyFence;
        for (int li = fb.headingLine0 + 1;
             li < fb.extentEnd0 && li < lines.size(); ++li) {
            const QString &bl = lines.at(li);
            if (!bodyFence.isNull()) {
                const QChar c = fenceOpenerChar(bl);
                if (!c.isNull() && c == bodyFence) bodyFence = QChar();
                continue;
            }
            const QChar opener = fenceOpenerChar(bl);
            if (!opener.isNull()) { bodyFence = opener; continue; }
            if (bulletArmRe().match(bl).hasMatch()) return true;
        }
        return false;
    };

    // Classify each line-less block (§2.4) and record the stamp insert points.
    // Reported `line` values are input coordinates (§2.6): the 1-based heading
    // line in the ORIGINAL file.
    QVector<int> insertAt0;  // 0-based indices in the original line list
    for (const FindingBlock &fb : blocks) {
        if (fb.idLine0 >= 0) continue;  // has a Proposed-ID line — backfill pass
        const int line1  = fb.headingLine0 + 1;
        const bool below = (watermark < 0) || (line1 > watermark);
        const bool shaped = isFindingShaped(fb);
        if (shaped && below) {
            res.stamped.append({ fb.heading, line1 });
            // Insertion point: directly under the heading, or — when the line
            // directly after it is blank — after that blank (preserve the
            // heading → blank → bullets layout, spec § 2.2 pass 2).
            int at0 = fb.headingLine0 + 1;
            if (at0 < lines.size() && lines.at(at0).trimmed().isEmpty()) ++at0;
            insertAt0.append(at0);
        } else if (shaped && !below) {
            res.orphans.append({ fb.heading, line1,
                QStringLiteral("finding_shaped_above_watermark") });
        } else if (!shaped && below) {
            res.unclassified.append({ fb.heading, line1 });
        }
        // not-shaped & above → expected already-reviewed prose: not reported.
    }

    // Apply bottom-up so an earlier insert never shifts a later index (parity
    // with compactResolved). Stamps are disjoint (one per finding).
    QStringList outLines = lines;

    // ANTS-3474 — backfill pass. A finding that ALREADY carries a BLANK
    // Proposed-ID line (the append_finding placeholder) gets it replaced in
    // place with a confident tracking-table id. Runs BEFORE the stamp inserts
    // so the idLine0 indices (from the original line list) stay valid — an
    // in-place replacement shifts nothing; the inserts below do.
    if (backfillFromTracking) {
        for (const FindingBlock &fb : blocks) {
            if (fb.idLine0 < 0) continue;               // no id line — stamp path
            const QString v = fb.idValue.trimmed();
            const bool blank = v.isEmpty()
                || v.contains(QStringLiteral("maintainer to assign"),
                              Qt::CaseInsensitive);
            if (!blank) continue;                       // already a real id
            const BackfillMatch m = matchId(fb.heading);
            if (m.id.isEmpty()) continue;               // no confident match
            outLines[fb.idLine0] =
                QStringLiteral("- **Proposed ID:** ") + m.id;
            res.backfilled.append(
                { fb.heading, fb.headingLine0 + 1, m.id, m.conf });
        }
    }

    std::sort(insertAt0.begin(), insertAt0.end(),
              [](int a, int b) { return a > b; });
    for (int at0 : insertAt0) outLines.insert(at0, kStamp);

    // Marker bump (pass 1), applied last. Re-locate the first marker line in
    // the stamped output (robust to a mis-placed marker that a below-it stamp
    // shifted) and replace it; insert a canonical line 1 when absent.
    static const QString kMarker =
        QStringLiteral("<!-- ants-mcp-feedback: 2 -->");
    if (markerPresent) {
        for (int i = 0; i < outLines.size(); ++i) {
            if (markerRe().match(outLines.at(i)).hasMatch()) {
                outLines[i] = kMarker;
                break;
            }
        }
    } else {
        outLines.insert(0, kMarker);
    }

    res.newContent = outLines.join(QLatin1Char('\n'));
    res.bytesDelta = static_cast<long>(res.newContent.toUtf8().size())
                   - static_cast<long>(content.toUtf8().size());
    return res;
}

// ANTS-3447 — assign_id. Fill ONE `### ` finding's `**Proposed ID:**` line:
// replace it (or its `_(maintainer to assign)_` placeholder / an existing id)
// with `- **Proposed ID:** <value>`, or insert that line as the finding's first
// body bullet when absent. Pure; single line replace or insert; no roadmap, no
// marker touch. The wrapper (cmdFeedbackLog) validates the request shape
// (hasIds XOR hasClosure, ^ANTS-[0-9]+$ ids, newline-folded closure) and owns
// the atomic write. Gates + invariants: docs/specs/ANTS-3447.md.
AssignResult assignId(const QString &content, const AssignTarget &target) {
    AssignResult res;
    const QStringList lines = content.split(QLatin1Char('\n'));
    const QVector<FindingBlock> blocks = enumerateFindingBlocks(lines);

    // Resolve the target `### ` finding — compact_shipped's gate SHAPE
    // (ANTS-3421 § 2.3) over the `### ` enumerator (spec § 2.2 step 1 / INV-4):
    // heading_line pins a repeated heading; a bare heading matching > 1 block is
    // target_ambiguous (+ candidates); 0 matches is target_not_found.
    const QString want = target.heading.trimmed();
    int idx = -1;   // index into `blocks`
    if (target.headingLine > 0) {
        const int wantLine0 = target.headingLine - 1;
        for (int k = 0; k < blocks.size(); ++k)
            if (blocks.at(k).headingLine0 == wantLine0) { idx = k; break; }
        // heading_line must name a `### ` heading equal to `heading`.
        if (idx < 0 ||
            (!want.isEmpty() && blocks.at(idx).heading.trimmed() != want)) {
            res.code = QStringLiteral("target_not_found");
            res.newContent = content;
            return res;
        }
    } else {
        QVector<int> matches;
        for (int k = 0; k < blocks.size(); ++k)
            if (blocks.at(k).heading.trimmed() == want) matches.append(k);
        if (matches.isEmpty()) {
            res.code = QStringLiteral("target_not_found");
            res.newContent = content;
            return res;
        }
        if (matches.size() > 1) {
            res.code = QStringLiteral("target_ambiguous");
            for (int k : matches)
                res.candidates.append(blocks.at(k).headingLine0 + 1);
            res.newContent = content;
            return res;
        }
        idx = matches.first();
    }

    const FindingBlock &fb = blocks.at(idx);
    res.heading = fb.heading;
    res.line    = fb.headingLine0 + 1;   // input coordinate (INV-2)
    res.value   = target.value;

    // Canonical form — byte-identical prefix to renderFindingBlock's, so the
    // enumerator + compact_resolved recognise the line (INV-3).
    const QString canonical =
        QStringLiteral("- **Proposed ID:** ") + target.value;

    QStringList outLines = lines;
    if (fb.idLine0 >= 0) {
        // Replace the finding's FIRST `**Proposed ID:**` line in place (any
        // stray second one is left as-is).
        res.inserted = false;
        if (lines.at(fb.idLine0) == canonical) {
            // Byte-identical no-op — skip the write so the mtime/ETag is
            // untouched (idempotency, §2.5 / INV-6/8).
            res.changed = false;
            res.newContent = content;
            return res;
        }
        outLines[fb.idLine0] = canonical;
        res.changed = true;
    } else {
        // No id line — insert as the first body bullet: directly under the
        // heading, or after a single blank line following it (preserve the
        // heading → blank → bullets layout, parity with migrate_v2 § 2.2 pass 2).
        // No finding-shaped gate: the maintainer named this block explicitly.
        res.inserted = true;
        res.changed  = true;
        int at0 = fb.headingLine0 + 1;
        if (at0 < lines.size() && lines.at(at0).trimmed().isEmpty()) ++at0;
        outLines.insert(at0, canonical);
    }

    res.newContent = outLines.join(QLatin1Char('\n'));
    res.bytesDelta = static_cast<long>(res.newContent.toUtf8().size())
                   - static_cast<long>(content.toUtf8().size());
    return res;
}

}  // namespace FeedbackFile
