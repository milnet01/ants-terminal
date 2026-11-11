// ANTS-1961 / ANTS-1962 — implementation of the shared feedback-file
// module. See feedbackfile.h + docs/standards/mcp-feedback-files.md.

#include "feedbackfile.h"

#include "markdownscan.h"

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

// ANTS-3603 — the fence primitives now live in MarkdownScan (markdownscan.h).
// Bring fenceOpenerChar into this namespace so the boundary scanners below
// call it unqualified, exactly as they did before the hoist.
using MarkdownScan::fenceOpenerChar;

// A boundary heading is exactly one or two leading hashes followed by a
// space (`###`+ are inert body lines). Any `# ` line counts: the v1 readers
// take the first boundary as the file's title, whatever it says. The v2
// scanner, enumerateFindingBlocks, accepts only a dated or title H1
// (ANTS-3695) and also stops at `### `.
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

// ANTS-6145 — true for each line inside a fenced block, opener and closer
// included. The one fence walk every scanner in this file reads, so a fence
// fix lands once. The rule is mcp-feedback-files.md's: a fence opens at up to
// 3 spaces of indent and closes on the same character, in a run at least as
// long (ANTS-4820). Not MarkdownScan::fenceMask, which also follows list
// items, a rule this format does not state.
QVector<bool> fencedLines(const QStringList &lines) {
    QVector<bool> fenced(lines.size(), false);
    QChar openFence;  // null when not inside a fence
    int   openFenceRun = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!openFence.isNull()) {
            fenced[i] = true;
            if (MarkdownScan::fenceCloses(line, openFence, openFenceRun))
                openFence = QChar();
            continue;
        }
        int openerRun = 0;
        const QChar opener = fenceOpenerChar(line, 3, &openerRun);
        if (!opener.isNull()) {
            fenced[i]    = true;
            openFence    = opener;
            openFenceRun = openerRun;
        }
    }
    return fenced;
}

// ANTS-3421 — single-source fence-aware boundary scan. One `#`/`## `
// heading outside a fenced region is a boundary; `###`+ are inert body
// lines. parse() (below) calls this, so the parser contract has exactly one
// implementation. Returns one Boundary per heading in document order.
struct Boundary {
    int     line0 = 0;         // 0-based line index of the heading
    QString text;          // the heading line, verbatim
    bool    isMaintainer = false;  // matches the maintainer anchor regex
};

QVector<Boundary> scanBoundaries(const QStringList &lines) {
    QVector<Boundary> out;
    const QVector<bool> fenced = fencedLines(lines);
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (fenced.at(i) || !isBoundaryHeading(line)) continue;
        out.append({i, line, isMaintainerHeading(line)});
    }
    return out;
}

// Exclusive 0-based end of boundary `bi`'s block: the next boundary of either
// level, or EOF. parse() bounds a maintainer block with this.
int blockEnd(const QVector<Boundary> &boundaries, int bi, int lineCount) {
    return (bi + 1 < boundaries.size()) ? boundaries.at(bi + 1).line0
                                        : lineCount;
}

// ANTS-3448 — version marker regex, digit-optional (`[0-9]*`) so a malformed
// `<!-- ants-mcp-feedback: -->` captures "" (→ version 0), not a parse error.
// Read by markerVersion().
const QRegularExpression &markerRe() {
    static const QRegularExpression re(
        QStringLiteral("<!--\\s*ants-mcp-feedback:\\s*([0-9]*)\\s*-->"));
    return re;
}

// ANTS-3448 — closure regex: a `**Proposed ID:**` value beginning the literal
// `n/a` followed by a word boundary (whitespace, dash, em-dash, or EOL) — so
// `n/architecture` is NOT a closure. parse()'s v2 branch reads it.
const QRegularExpression &closureRe() {
    static const QRegularExpression re(
        QStringLiteral("^n/a\\b"), QRegularExpression::CaseInsensitiveOption);
    return re;
}

// ANTS-3631 — the awaiting marker: a `**Proposed ID:**` value the maintainer
// filled with a QUESTION for the reporting session. Un-triaged on purpose, so
// the finding stays in that session's `feedback_query` delta carrying the
// question — every other way of attaching one also fills the slot, which is
// exactly what removes the finding from the surface its reader looks at.
//
// Beside closureRe(): parse()'s classifier and the question extractor must
// not drift on what counts as a marker.
//
// TESTED BEFORE THE CLOSURE AND ID TESTS, and that ordering is the contract
// rather than an implementation detail. A question naturally quotes an id —
// "is this the same as ANTS-1234?" — so an id-first classifier reads the
// finding as triaged, drops it from the delta, adds a never-assigned id to
// mapped_ids, and hands compact_resolved a shippable-looking finding. The
// question is then silently lost, which is the whole defect this disposition
// exists to prevent.
//
// The classifier deliberately anchors on the PREFIX only, not on the em-dash
// or the closing `)_`: classification is the safety property and must fail
// toward "still un-triaged", while question extraction is display text and may
// fail soft to an empty string (standard § Maintainer triage).
const QRegularExpression &awaitingRe() {
    static const QRegularExpression re(
        QStringLiteral("^_\\(awaiting reporter\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// The question inside `_(awaiting reporter — <question>)_`: the span between
// the em-dash and the trailing `)_`, trimmed. Empty when either is absent,
// which the marker's classification does not depend on.
QString awaitingQuestion(const QString &value) {
    if (!awaitingRe().match(value).hasMatch()) return {};
    static const QRegularExpression re(
        QString::fromUtf8("^_\\(awaiting reporter\\s*\xE2\x80\x94\\s*(.*?)\\)_\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(value.trimmed());
    return m.hasMatch() ? m.captured(1).trimmed() : QString();
}

// ANTS-3448 — finding-bullet arm: a `- **What/Repro/Impact:**` (or `**What**:`)
// body bullet: what makes an id-less `### ` block finding-shaped in parse()'s
// suspected_untagged scan.
const QRegularExpression &bulletArmRe() {
    static const QRegularExpression re(
        QStringLiteral("^ *[-*] +\\*\\*(what|repro|impact)[:*]"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// ANTS-5108 — split a tracking-table row on its unescaped `|` and turn each
// `\|` back into `|`, the inverse of escapeTrackingCell. Splitting on every
// `|` broke a written row into extra cells, shifting status and notes.
QStringList splitTrackingRow(const QString &row) {
    QStringList cells;
    QString cell;
    for (int i = 0; i < row.size(); ++i) {
        const QChar c = row.at(i);
        if (c == QLatin1Char('\\') && i + 1 < row.size() &&
            row.at(i + 1) == QLatin1Char('|')) {
            cell += QLatin1Char('|');
            ++i;
        } else if (c == QLatin1Char('|')) {
            cells.append(cell);
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells.append(cell);
    return cells;
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

    // Pass 1 — every non-fenced boundary heading, via the shared scanner
    // (ANTS-3421 extraction). Both versions report the maintainer blocks.
    const QVector<Boundary> boundaries = scanBoundaries(lines);
    int lastMaintainerIdx = -1;          // 0-based line index
    for (const Boundary &b : boundaries) {
        if (!b.isMaintainer) continue;
        ++r.maintainerBlockCount;
        lastMaintainerIdx = b.line0;
    }
    r.lastMaintainerLine =
        (lastMaintainerIdx >= 0) ? (lastMaintainerIdx + 1) : -1;

    // Pass 2 — each maintainer block (its heading to blockEnd): its tracking
    // rows, on both versions, and its ANTS-[0-9]+ ids, which are v1's
    // mappedIds. Fenced regions inside the body are still scanned for IDs —
    // the fence rule only governs boundary detection, not ID extraction; an
    // ID pasted inside a maintainer-block fence is still a mapped ID.
    static const QRegularExpression idRe(QStringLiteral("ANTS-[0-9]+"));
    // ANTS-3445 — anchored whole-token id test for the tracking-row ID column
    // (a cell holds only a real id, never an id embedded in prose).
    static const QRegularExpression idCellRe(QStringLiteral("^ANTS-[0-9]+$"));
    // ANTS-3371 — separator row of a GFM table (`|---|:--:|` etc.).
    static const QRegularExpression sepCellRe(QStringLiteral("^:?-{1,}:?$"));
    QSet<QString> v1Ids;
    for (int bi = 0; bi < boundaries.size(); ++bi) {
        if (!boundaries.at(bi).isMaintainer) continue;
        const int start = boundaries.at(bi).line0;
        const int end = blockEnd(boundaries, bi, int(lines.size()));
        for (int li = start; li < end; ++li) {
            const QString &row = lines.at(li);
            auto it = idRe.globalMatch(row);
            while (it.hasNext()) v1Ids.insert(it.next().captured(0));

            // ANTS-3371 — extract the maintainer tracking-table data rows
            // so include_tracking can return per-item status without a
            // full-file Read + hand-parse. A pipe-table row trims to a
            // leading `|`; split on `|`, drop the empty leading/trailing
            // cells, skip the header (`Item …`) and `|---|` separator.
            const QString trimmed = row.trimmed();
            if (!trimmed.startsWith(QLatin1Char('|'))) continue;
            QStringList cells = splitTrackingRow(trimmed);
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

    // ANTS-3448 — the delta / mappedIds / suspectedUntagged fields branch on
    // the format version. The v1 boundary scan above (lastMaintainerLine /
    // maintainerBlockCount / trackingRows) already ran on both versions (INV-6).
    // The v2 branch returns; the v1 rule follows it.
    if (v2) {
        // --- v2 rule (spec § 2.2): the un-triaged delta is the ordered
        // concatenation of every `### ` finding whose first `**Proposed ID:**`
        // value is neither an ANTS-NNNN id nor an `n/a` closure. ------------
        const QVector<FindingBlock> blocks = enumerateFindingBlocks(lines);

        // Fence-aware body scan: does this line-less block carry a finding
        // bullet (`- **What/Repro/Impact:**`)? → suspected_untagged (§2.2/5).
        const QVector<bool> fenced = fencedLines(lines);
        auto bodyHasFindingBullet = [&](const FindingBlock &fb) -> bool {
            for (int li = fb.headingLine0 + 1;
                 li < fb.extentEnd0 && li < lines.size(); ++li) {
                if (!fenced.at(li) && bulletArmRe().match(lines.at(li)).hasMatch())
                    return true;
            }
            return false;
        };

        // ANTS-4513 — a finding's record, carrying its enclosing entry
        // heading: the last boundary above it.
        auto record = [&](const FindingBlock &fb, const QString &triage) {
            FindingRecord rec;
            rec.heading = fb.heading;
            rec.line    = fb.headingLine0 + 1;
            rec.triage  = triage;
            // ANTS-6733 L14-20 — "" and not a null QString, which binds as
            // SQL NULL into the NOT NULL entry_heading column.
            rec.entryHeading = QStringLiteral("");
            for (const Boundary &b : boundaries) {
                if (b.line0 >= fb.headingLine0) break;
                rec.entryHeading = b.text;
            }
            static const QRegularExpression dateRe(
                QStringLiteral("^#+\\s+(\\d{4}-\\d{2}-\\d{2})"));
            const auto dm = dateRe.match(rec.entryHeading);
            if (dm.hasMatch()) rec.filedOn = dm.captured(1);
            r.findings.append(rec);
            return &r.findings.last();
        };

        QSet<QString> v2ids;
        QStringList deltaBlocks;     // one `\n`-joined block per un-triaged finding
        int deltaLineTotal = 0;
        for (const FindingBlock &fb : blocks) {
            if (fb.idLine0 < 0) {
                // No id line → not a finding. A finding-shaped one is a
                // hand-editor silent-loss risk (suspected_untagged); bare prose
                // is ignored.
                if (bodyHasFindingBullet(fb)) {
                    r.suspectedUntagged.append({ fb.heading, fb.headingLine0 + 1 });
                    record(fb, QStringLiteral("untagged"));
                }
                continue;
            }
            const QString val = fb.idValue.trimmed();
            // ANTS-3631 — the three tests, in this order. Awaiting FIRST,
            // before the id test, because a question quoting an id would
            // otherwise classify as triaged: see awaitingRe()'s header for the
            // loss that produces. It contributes no ids for the same reason a
            // closure does not — an id inside a question was never assigned.
            const bool isAwaiting = awaitingRe().match(val).hasMatch();
            if (isAwaiting)
                r.awaiting.append({ fb.heading, fb.headingLine0 + 1,
                                    awaitingQuestion(val) });
            const bool isClosure = closureRe().match(val).hasMatch();
            const bool hasId = idRe.match(val).hasMatch();
            if (!isAwaiting && !isClosure && hasId) {
                // Triaged with real id(s): contribute them to mappedIds.
                FindingRecord *rec = record(fb, QStringLiteral("mapped"));
                auto it = idRe.globalMatch(val);
                while (it.hasNext()) {
                    const QString id = it.next().captured(0);
                    v2ids.insert(id);
                    if (!rec->ids.contains(id)) rec->ids.append(id);
                }
                continue;   // triaged → not in the delta
            }
            if (!isAwaiting && isClosure) {
                record(fb, QStringLiteral("closed"));
                continue;  // closure → triaged, contributes no ids
            }
            record(fb, isAwaiting ? QStringLiteral("awaiting")
                                  : QStringLiteral("untriaged"));
            // Un-triaged: empty, the placeholder, any non-id non-n/a text, or
            // an awaiting marker — which is un-triaged DELIBERATELY, so the
            // question reaches the reporter in the surface they already read.
            // The finding's block, all trailing blank lines stripped per block.
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
        // ANTS-3744 — a fully-condensed file (ANTS-3475) keeps its ids only on
        // a `## Tracked in ROADMAP (detail + status there): ANTS-…` pointer
        // line, with no finding blocks left to carry a `**Proposed ID:**`
        // slot. Without this branch such a file reports mapped_ids:[], so the
        // reporting session's documented way of learning that its item shipped
        // (mapped_id_status) returns nothing for exactly the files tidied
        // hardest. Only consulted when the finding blocks yielded no id, so a
        // file that still carries write-ups keeps its existing harvest.
        if (v2ids.isEmpty()) {
            static const QRegularExpression trackedLineRe(
                QStringLiteral(R"(^#{1,6}\s+Tracked in ROADMAP\b)"));
            for (const QString &l : lines) {
                if (!trackedLineRe.match(l).hasMatch()) continue;
                auto it = idRe.globalMatch(l);
                while (it.hasNext()) v2ids.insert(it.next().captured(0));
            }
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
    r.mappedIds = QStringList(v1Ids.begin(), v1Ids.end());
    r.mappedIds.sort();

    // Delta start: the first contributor heading strictly after the last
    // maintainer heading. With no maintainer heading, the first boundary
    // heading is the H1 title and the delta starts on the line after it.
    // No such heading → no delta.
    int deltaStartIdx = -1;  // 0-based line index of the delta's first line
    for (const Boundary &b : boundaries) {
        if (b.isMaintainer) continue;
        if (lastMaintainerIdx < 0) { deltaStartIdx = b.line0 + 1; break; }
        if (b.line0 > lastMaintainerIdx) { deltaStartIdx = b.line0; break; }
    }

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

// ANTS-3695 — a field value is contributor prose, and a Repro value is a
// shell transcript by nature: a `#` comment at column 0 is the most
// ordinary thing in one. Rendered flush-left it reads as an H1, which ends
// the enclosing `###` finding block — taking the `- **Proposed ID:**` line
// the verb itself just wrote outside the finding, so feedback_query drops
// the whole thing from the delta. Indenting every continuation line by two
// spaces keeps it inside the list item and neutralises the entire class
// (hash, pipe, hyphen, fence, setext underline) while rendering
// identically. The first line needs nothing — it already sits after the
// `- **Field:** ` label.
QString indentContinuation(const QString &value) {
    QString v = value;
    v.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return v.replace(QStringLiteral("\n"), QStringLiteral("\n  "));
}

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
        // ANTS-3695 — a heading is one line by definition; a newline in the
        // title would put the rest of it outside the block it names.
        QString title = f.title;
        title.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
        title.replace(QLatin1Char('\n'), QLatin1Char(' '));
        out += QStringLiteral("\n### ") + title + QStringLiteral("\n\n");
        if (!f.what.isEmpty())
            out += QStringLiteral("- **What:** ") + indentContinuation(f.what) +
                   QStringLiteral("\n");
        if (!f.repro.isEmpty())
            out += QStringLiteral("- **Repro:** ") + indentContinuation(f.repro) +
                   QStringLiteral("\n");
        if (!f.impact.isEmpty())
            out += QStringLiteral("- **Impact:** ") +
                   indentContinuation(f.impact) + QStringLiteral("\n");
        if (!f.suggestedFix.isEmpty())
            out += QStringLiteral("- **Suggested fix:** ") +
                   indentContinuation(f.suggestedFix) + QStringLiteral("\n");
        // Always emitted blank — the contributor never assigns an ID
        // (ANTS-1962 INV-6).
        out += QStringLiteral("- **Proposed ID:** _(maintainer to assign)_\n");
    }
    return out;
}

QString skeleton(const QString &projectTitle) {
    // ANTS-3476 — a brand-new file is born v2 (inline-ID): the `: 2` marker +
    // the v2 banner verbatim from docs/standards/mcp-feedback-files.md
    // §"File skeleton". No migrate_v2 is ever needed on a fresh file. The v1
    // marker + append_tracking banner are retained only on legacy/un-migrated
    // corpus files.
    QString out;
    out += QStringLiteral("<!-- ants-mcp-feedback: 2 -->\n");
    out += QStringLiteral("# Ants MCP Feedback — ") + projectTitle +
           QStringLiteral("\n\n");
    out += QStringLiteral(
        "> Format: docs/standards/mcp-feedback-files.md in the Ants "
        "Terminal repo.\n"
        "> **Contributors (ANTS-2226):** read new items with the "
        "`feedback_query`\n"
        "> Ants-MCP verb (the un-triaged tail) and append findings with "
        "`feedback_log\n"
        "> op:append_finding` — don't hand-edit. Each finding carries a "
        "blank\n"
        "> `**Proposed ID:**` line; leave it blank — the maintainer fills "
        "it. Never\n"
        "> assign an ANTS-NNNN id yourself.\n");
    return out;
}

// ANTS-3443 — fence-aware `### `-block enumerator, shared by parse()'s v2
// delta and assign_id. A block runs from its `### ` heading to the next
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
    // 1–3 hash boundary (`# `/`## `/`### `). Two differences from the v1
    // scanBoundaries: a `### ` also terminates a block, and only a dated or
    // title H1 is a boundary (below). See isBoundaryHeading for why v1 keeps
    // every H1.
    static const QRegularExpression boundaryRe(QStringLiteral("^#{1,3} "));
    // ANTS-3695 — read-side hardening for files written before the
    // continuation-indent fix: a column-0 `#` pasted from a shell transcript
    // is not a heading, but `^#{1,3} ` cannot tell it from one, and treating
    // it as a boundary orphans the rest of the finding (including its
    // Proposed ID line). Every H1 this format actually emits comes from our
    // own renderer — a `# <ISO date>` session heading or the skeleton's
    // `# Ants MCP Feedback — <project>` title — so an H1 that is neither is
    // body text. NOT "never bound at an H1" as first proposed: h1Heading mode
    // renders real session headings at that depth, and ignoring those would
    // run a block into the next session's findings. `##`/`###` are unchanged.
    static const QRegularExpression h1BoundaryRe(QStringLiteral(
        "^# (?:\\d{4}-\\d{2}-\\d{2}|Ants MCP Feedback\\b)"));

    // Pass 1 — every boundary heading outside a fence, in document order.
    struct B { int line0; bool isSub; };   // isSub ⟹ a `### ` heading
    QVector<B> bounds;
    const QVector<bool> fenced = fencedLines(lines);
    for (int i = 0; i < lines.size(); ++i) {
        const QString &l = lines.at(i);
        if (fenced.at(i) || !boundaryRe.match(l).hasMatch()) continue;
        const bool isH1 = l.startsWith(QStringLiteral("# "));
        if (isH1 && !h1BoundaryRe.match(l).hasMatch()) continue;  // ANTS-3695
        bounds.append({ i, l.startsWith(QStringLiteral("### ")) });
    }

    // Pass 2 — for each `### ` boundary emit a block; extent = next boundary
    // of any level (or EOF).
    QVector<FindingBlock> out;
    for (int bi = 0; bi < bounds.size(); ++bi) {
        if (!bounds.at(bi).isSub) continue;
        FindingBlock fb;
        fb.headingLine0 = bounds.at(bi).line0;
        fb.heading      = lines.at(fb.headingLine0);
        fb.extentEnd0   = (bi + 1 < bounds.size()) ? bounds.at(bi + 1).line0
                                                   : lines.size();
        for (int li = fb.headingLine0 + 1; li < fb.extentEnd0; ++li) {
            if (fenced.at(li)) continue;
            const auto m = idLineRe.match(lines.at(li));
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

// ANTS-3504 — a roadmap bullet's ship-date: the ISO date on the LAST body line
// beginning (after indent) with a `Resolved` marker, the date with or without
// parentheses (`Resolved (2026-07-09):` and `Resolved 2026-05-29:` both occur).
// MultilineOption anchors `^` at each body line; the last match wins (a bullet
// may carry earlier Progress/Resolved updates). "" when nothing matches — a
// mid-line `… . Resolved <date> by …` or `Resolved as of <date>` is a graceful
// miss (dateless fallback), per docs/specs/ANTS-3504.md § 2.1.
QString shipDateFromRoadmapBody(const QString &roadmapBulletBody) {
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*Resolved\s+\(?(\d{4}-\d{2}-\d{2}))"),
        QRegularExpression::MultilineOption);
    QString last;
    auto it = re.globalMatch(roadmapBulletBody);
    while (it.hasNext()) last = it.next().captured(1);
    return last;
}

// ANTS-4646(b) — rewrite the H1's project name, preserving the file's own
// prefix casing. The corpus carries both "Ants MCP feedback" and
// "Ants MCP Feedback"; this renames a project, it does not normalise a corpus.
SetTitleResult setTitle(const QString &content, const QString &projectTitle) {
    SetTitleResult out;
    out.newContent = content;
    out.newTitle   = projectTitle.trimmed();

    QStringList lines = content.split(QLatin1Char('\n'));
    int h1 = -1;
    for (const Boundary &b : scanBoundaries(lines)) {
        // scanBoundaries already skips fenced regions, so a `# ` inside a code
        // block cannot be picked up here.
        if (b.text.startsWith(QStringLiteral("# "))) { h1 = b.line0; break; }
    }
    if (h1 < 0) {
        // Never invent one: the format fixes the H1's position, and a guess
        // would put it somewhere the reader does not expect.
        out.code = QStringLiteral("no_h1");
        return out;
    }

    const QString line = lines.at(h1);
    static const QString kDash = QString::fromUtf8("\xE2\x80\x94");   // —
    const int dash = line.lastIndexOf(kDash);
    const QString rebuilt = dash >= 0
        ? line.left(dash + kDash.size()) + QLatin1Char(' ') + out.newTitle
        : QStringLiteral("# Ants MCP Feedback ") + kDash + QLatin1Char(' ')
              + out.newTitle;
    out.oldTitle = dash >= 0
        ? line.mid(dash + kDash.size()).trimmed()
        : line.mid(2).trimmed();

    if (rebuilt == line) return out;        // changed stays false — no write
    lines[h1]      = rebuilt;
    out.newContent = lines.join(QLatin1Char('\n'));
    out.changed    = true;
    return out;
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
    const auto notFound = [&] {
        res.code = QStringLiteral("target_not_found");
        for (const FindingBlock &b : blocks) res.headings.append(b.heading.trimmed());
        res.newContent = content;
        return res;
    };
    int idx = -1;   // index into `blocks`
    if (target.headingLine > 0) {
        const int wantLine0 = target.headingLine - 1;
        for (int k = 0; k < blocks.size(); ++k)
            if (blocks.at(k).headingLine0 == wantLine0) { idx = k; break; }
        // heading_line must name a `### ` heading equal to `heading`.
        if (idx < 0 ||
            (!want.isEmpty() && blocks.at(idx).heading.trimmed() != want))
            return notFound();
    } else {
        QVector<int> matches;
        for (int k = 0; k < blocks.size(); ++k)
            if (blocks.at(k).heading.trimmed() == want) matches.append(k);
        if (matches.isEmpty()) return notFound();
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
    int idPos = -1;   // final index of the id line in outLines
    if (fb.idLine0 >= 0) {
        // Replace the finding's FIRST `**Proposed ID:**` line in place (any
        // stray second one is left as-is). A byte-identical assignment is a
        // harmless no-op here; the overall `changed` flag is decided by the
        // final content comparison below so a note-only change still commits.
        res.inserted = false;
        outLines[fb.idLine0] = canonical;
        idPos = fb.idLine0;
    } else {
        // No id line — insert as the first body bullet: directly under the
        // heading, or after a single blank line following it (preserve the
        // heading → blank → bullets layout, parity with migrate_v2 § 2.2 pass 2).
        // No finding-shaped gate: the maintainer named this block explicitly.
        res.inserted = true;
        int at0 = fb.headingLine0 + 1;
        if (at0 < lines.size() && lines.at(at0).trimmed().isEmpty()) ++at0;
        outLines.insert(at0, canonical);
        idPos = at0;
    }

    // ANTS-3571 — the documented `note` param was silently dropped. Render it
    // as a single `- **Note:** <note>` bullet directly under the Proposed ID
    // line (the wrapper folds newlines to spaces, so it stays one line).
    // Replace an existing note bullet in the SAME finding rather than append,
    // so a re-assign with the same id+note is a byte-identical no-op (INV-8).
    if (!target.note.isEmpty()) {
        const QString canonNote =
            QStringLiteral("- **Note:** ") + target.note;
        static const QRegularExpression noteRe(
            QStringLiteral("^- \\*\\*note:\\*\\*"),
            QRegularExpression::CaseInsensitiveOption);
        const int shift   = res.inserted ? 1 : 0;  // an id insert shifted the tail
        const int bodyEnd = fb.extentEnd0 + shift;  // exclusive, outLines coords
        int noteLine = -1;
        for (int li = fb.headingLine0 + 1;
             li < bodyEnd && li < outLines.size(); ++li) {
            if (li == idPos) continue;              // never the id line
            if (noteRe.match(outLines.at(li)).hasMatch()) { noteLine = li; break; }
        }
        if (noteLine >= 0) outLines[noteLine] = canonNote;   // replace in place
        else               outLines.insert(idPos + 1, canonNote);
        res.noteWritten = true;
    }

    res.newContent = outLines.join(QLatin1Char('\n'));
    // INV-8 idempotency spans BOTH the id line and the note bullet: `changed`
    // is the final byte comparison, so an unchanged file skips the write.
    res.changed    = (res.newContent != content);
    res.bytesDelta = static_cast<long>(res.newContent.toUtf8().size())
                   - static_cast<long>(content.toUtf8().size());
    return res;
}

}  // namespace FeedbackFile
