// ANTS-1548: see changeloglog.h.

#include "changeloglog.h"

#include <QStringList>
#include <QVector>

#include <algorithm>
#include <limits>

namespace ChangelogLog {

// Canonical Keep-a-Changelog category order (spec § sections).
// Public since ANTS-3533 (declared in changeloglog.h) so changelog_query
// can reuse it for the `bad_category` `accepted[]` echo.
const QStringList &canonicalCategories() {
    static const QStringList v = {
        QStringLiteral("Added"),   QStringLiteral("Changed"),
        QStringLiteral("Deprecated"), QStringLiteral("Removed"),
        QStringLiteral("Fixed"),   QStringLiteral("Security"),
    };
    return v;
}

QString kindToCategory(const QString &kind) {
    // Added — net-new surface.
    if (kind == QLatin1String("feature") ||
        kind == QLatin1String("implement") ||
        kind == QLatin1String("enhancement")) {
        return QStringLiteral("Added");
    }
    // Fixed — bug / regression / audit / review fixes.
    if (kind == QLatin1String("fix") ||
        kind == QLatin1String("doc-fix") ||
        kind == QLatin1String("audit-fix") ||
        kind == QLatin1String("review-fix")) {
        return QStringLiteral("Fixed");
    }
    // Security — its own Keep-a-Changelog category.
    if (kind == QLatin1String("security")) {
        return QStringLiteral("Security");
    }
    // Everything else (refactor/perf/optimize/chore/test/doc/release/
    // package/marketing/ux/investigate/research/accessibility) is a
    // change to existing behaviour.
    return QStringLiteral("Changed");
}

bool isValidCategory(const QString &category) {
    return canonicalCategories().contains(category);
}

namespace {
// ANTS-2125 — scan the Unreleased section [sectionStart, sectionEnd) for
// non-heading prose wedged between `###` category blocks. Returns the
// 1-based line of the first offending line, or -1 if the section is
// clean. Scanning starts only once the first `### ` category heading is
// seen, so a legitimate description paragraph directly under
// `## [Unreleased]` (before any category) is NOT flagged — only prose
// interleaved between or after category blocks (the stray-footer shape
// that motivated ANTS-2125) trips it. Within the category region the
// only legal non-heading content is list items and their indented
// continuations; anything else (a `---` rule, a flush-left paragraph) is
// the malformation we warn about.
//
// ANTS-4103 — two shapes it used to call malformed and should not, both
// reported attached to a SUCCESSFUL write, so they read as "the write worked
// but your file is broken" and invite a restructuring edit to a file that
// needed none:
//   • An HTML comment. It does not render, so it is not prose a reader ever
//     sees wedged between blocks. Tracked across lines so a multi-line
//     comment block is skipped whole.
//   • Prose sitting between a `### ` heading and that block's FIRST bullet.
//     That is the heading's own description — and it is exactly what this
//     verb's own op:add_subsection writes (a dated topic heading, blank,
//     flush-left body prose, blank, then the bullets), so the scanner was
//     flagging its canonical output. The stray-footer shape ANTS-2125 exists
//     for arrives AFTER a block's bullets, and still trips: its fixture puts
//     the `---` two lines below `### Added`'s bullet.
int firstInterleavedProseLine(const QStringList &lines,
                              int sectionStart, int sectionEnd) {
    bool sawCategory = false;
    bool sawBulletInBlock = false;   // ANTS-4103 — reset at each `### `
    bool insideComment   = false;    // ANTS-4103
    for (int i = sectionStart; i < sectionEnd && i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const QString t = raw.trimmed();
        if (insideComment) {                        // ANTS-4103
            if (t.contains(QStringLiteral("-->"))) insideComment = false;
            continue;
        }
        if (t.startsWith(QStringLiteral("<!--"))) { // ANTS-4103
            if (!t.contains(QStringLiteral("-->"))) insideComment = true;
            continue;
        }
        if (t.isEmpty()) continue;                  // blank spacer
        if (t.startsWith(QLatin1Char('#'))) {       // any heading line
            if (t.startsWith(QStringLiteral("### "))) {
                sawCategory      = true;
                sawBulletInBlock = false;           // ANTS-4103 — new block
            }
            continue;
        }
        if (!sawCategory) continue;                 // pre-category preamble
        // A list item is a `-`/`*`/`+` marker followed by a space; a bare
        // run like `---`/`***` is a thematic break (the stray-footer
        // separator), NOT a bullet, so it must still trip the advisory.
        if (t.size() >= 2 && t.at(1) == QLatin1Char(' ') &&
            (t.at(0) == QLatin1Char('-') || t.at(0) == QLatin1Char('*') ||
             t.at(0) == QLatin1Char('+'))) {
            sawBulletInBlock = true;                // ANTS-4103
            continue;
        }
        if (raw.startsWith(QLatin1Char(' ')) ||     // indented continuation
            raw.startsWith(QLatin1Char('\t'))) continue;
        if (!sawBulletInBlock) continue;            // ANTS-4103 — block intro
        return i + 1;                               // 1-based, for humans
    }
    return -1;
}

// ANTS-3416 — detect a FEATURE-GROUPED `## [Unreleased]` section: one whose
// direct `### ` children are dated topic headings
// (`### <id> — <topic> (<date>)`, newest-first) with `**Bold**` category
// runs (`**Fixed**`, `**Security:**`) beneath them — the MAME Curator house
// style — rather than flat Keep-a-Changelog `### <category>` blocks.
// changelog_log models `### ` as the category slot, so a flat-category
// insert here lands the entry as a sibling of the dated topics at the
// section END, breaking the every-heading-is-a-dated-topic invariant (it
// then has to be hand-deleted). Returns the 1-based line of the first
// dated-topic heading (for the refusal message), or -1 for a normal layout.
//
// All three signals are required, to keep this a precise refusal (a false
// positive would block a legitimate insert):
//   (1) ≥1 `### ` heading in the section;
//   (2) NONE of the `### ` headings is a canonical category word — a single
//       canonical heading means it is a flat (possibly messy) category
//       layout, handled by the normal insert + the ANTS-2125 advisory, not
//       feature-grouped;
//   (3) ≥1 flush-left `**Bold**` run line (the category runs sit one level
//       BELOW the topic heading). A normal bullet `- **summary**` trims to a
//       leading `-`, not `**`, so it never trips this.
int firstFeatureGroupedTopicLine(const QStringList &lines,
                                 int sectionStart, int sectionEnd) {
    int firstTopic = -1;
    int headingCount = 0;
    bool sawBoldRun = false;
    for (int i = sectionStart; i < sectionEnd && i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.startsWith(QStringLiteral("### "))) {
            ++headingCount;
            const QString name = t.mid(4).trimmed();
            if (canonicalCategories().contains(name, Qt::CaseInsensitive))
                return -1;  // a real category heading → not feature-grouped
            if (firstTopic < 0) firstTopic = i + 1;  // 1-based, for humans
        } else if (t.startsWith(QStringLiteral("**"))) {
            sawBoldRun = true;
        } else if (t.startsWith(QStringLiteral("- **")) ||
                   t.startsWith(QStringLiteral("* **"))) {
            // ANTS-3803 (reported by Vestige) — a LIST-ITEM bold run counts.
            // ANTS-3416 tuned this detector on MAME Curator's layout, whose
            // category runs are flush-left `**Bold**` lines, so `t.startsWith
            // ("**")` was the whole test. But op:add_subsection (ANTS-3584)
            // writes `### <date> <Category> — <headline>` followed by
            // `- **summary** (id)` bullets, whose bold runs are list items —
            // the trimmed line starts `- `, sawBoldRun stayed false, and the
            // detector returned -1 on the very format this verb produces.
            //
            // The consequence was not cosmetic: with no canonical `###`
            // category heading present either, the flat insert then appended
            // the entry at the END of [Unreleased] — reported as ~10,000 lines
            // below the top of an 11,124-line file, where no reader looks.
            sawBoldRun = true;
        }
    }
    return (headingCount >= 1 && sawBoldRun) ? firstTopic : -1;
}
}  // namespace

QString formatBullet(const QString &summary, const QString &body,
                     const QString &id) {
    QString head = summary.trimmed();
    QString out = QStringLiteral("- **") + head + QStringLiteral("**");
    if (!id.isEmpty()) {
        out += QStringLiteral(" (") + id.trimmed() + QStringLiteral(")");
    }
    if (!body.trimmed().isEmpty()) {
        const QStringList lines = body.split(QLatin1Char('\n'));
        for (const QString &ln : lines) {
            const QString t = ln.trimmed();
            out += QLatin1Char('\n');
            if (!t.isEmpty()) out += QStringLiteral("  ") + t;
        }
    }
    return out;
}

InsertResult insertUnreleasedEntry(const QString &markdown,
                                   const QString &category,
                                   const QString &bulletBlock) {
    InsertResult r;
    if (!isValidCategory(category)) {
        r.code = QStringLiteral("bad_category");
        r.error = QStringLiteral(
            "changelog_log: \"%1\" is not a Keep-a-Changelog category "
            "(Added/Changed/Deprecated/Removed/Fixed/Security)")
                .arg(category);
        return r;
    }

    QStringList lines = markdown.split(QLatin1Char('\n'));

    // 1. Locate `## [Unreleased]`.
    int unrel = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.compare(QStringLiteral("## [Unreleased]"),
                      Qt::CaseInsensitive) == 0) {
            unrel = i;
            break;
        }
    }
    if (unrel < 0) {
        r.code = QStringLiteral("not_unreleased");
        r.error = QStringLiteral(
            "changelog_log: no `## [Unreleased]` heading found — the "
            "CHANGELOG must follow Keep-a-Changelog with an Unreleased "
            "section at the top");
        return r;
    }

    // 2. Bound the Unreleased section [unrel+1, sectionEnd).
    int sectionEnd = lines.size();
    for (int i = unrel + 1; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QStringLiteral("## "))) {
            sectionEnd = i;
            break;
        }
    }

    // ANTS-3416 — refuse a FEATURE-GROUPED section (dated `### ` topics +
    // `**Bold**` category runs, not flat Keep-a-Changelog categories). A
    // flat-category insert would land the entry as a sibling of the dated
    // topics at the section END, breaking the house style — so refuse and
    // let the caller hand-edit from the start rather than delete + re-add.
    const int topicLine =
        firstFeatureGroupedTopicLine(lines, unrel + 1, sectionEnd);
    if (topicLine > 0) {
        r.code = QStringLiteral("feature_grouped_section");
        r.error = QStringLiteral(
            "changelog_log: `## [Unreleased]` is feature-grouped — its "
            "`### ` subsections are dated topics (first at line %1), not "
            "Keep-a-Changelog categories, with `**Bold**` category runs "
            "beneath. A flat `### %2` insert would land as a sibling of the "
            "dated topics at the section end, breaking the house style. "
            "Hand-edit CHANGELOG.md: add the note under a new or existing "
            "`### <id> — <topic> (<date>)` subsection.")
            .arg(topicLine).arg(category);
        return r;
    }

    // ANTS-2125 — flag a pre-existing malformed section (non-heading
    // prose between category blocks) on the original body, before the
    // insert. Non-blocking: insertion proceeds regardless.
    const int proseLine =
        firstInterleavedProseLine(lines, unrel + 1, sectionEnd);
    if (proseLine > 0) {
        r.malformed_section = true;
        r.malformed_line = proseLine;
    }

    // 3. Find the `### <category>` heading within the section, and
    //    record the first later-ordered category heading (for ordered
    //    creation when ours is absent).
    const int wantOrder = canonicalCategories().indexOf(category);
    int catHeading = -1;
    int laterHeading = -1;  // first ### whose order > wantOrder
    for (int i = unrel + 1; i < sectionEnd; ++i) {
        const QString t = lines.at(i).trimmed();
        if (!t.startsWith(QStringLiteral("### "))) continue;
        const QString name = t.mid(4).trimmed();
        if (name.compare(category, Qt::CaseInsensitive) == 0) {
            catHeading = i;
            break;
        }
        const int ord = canonicalCategories().indexOf(name);
        if (ord > wantOrder && laterHeading < 0) laterHeading = i;
    }

    const QStringList bulletLines = bulletBlock.split(QLatin1Char('\n'));

    if (catHeading >= 0) {
        // Insert at the top of the existing category: right after the
        // heading line and its single blank spacer (if present).
        int insertAt = catHeading + 1;
        if (insertAt < sectionEnd && lines.at(insertAt).trimmed().isEmpty())
            ++insertAt;
        QStringList block = bulletLines;
        block.append(QString());  // blank line after the new bullet
        for (int k = 0; k < block.size(); ++k)
            lines.insert(insertAt + k, block.at(k));
        r.ok = true;
        r.markdown = lines.join(QLatin1Char('\n'));
        r.line = insertAt + 1;
        return r;
    }

    // 4. Category heading absent — create it in canonical order.
    int headingAt = (laterHeading >= 0) ? laterHeading : sectionEnd;
    QStringList block;
    block.append(QStringLiteral("### ") + category);
    block.append(QString());
    block += bulletLines;
    block.append(QString());
    for (int k = 0; k < block.size(); ++k)
        lines.insert(headingAt + k, block.at(k));
    r.ok = true;
    r.created_category = true;
    r.markdown = lines.join(QLatin1Char('\n'));
    r.line = headingAt + 3;  // 1-based line of the inserted bullet
    return r;
}

SubsectionResult insertUnreleasedSubsection(const QString &markdown,
                                            const QString &date,
                                            const QString &category,
                                            const QString &headline,
                                            const QString &body,
                                            const QStringList &bulletBlocks) {
    SubsectionResult r;
    if (!isValidCategory(category)) {
        r.code = QStringLiteral("bad_category");
        r.error = QStringLiteral(
            "changelog_log: \"%1\" is not a Keep-a-Changelog category "
            "(Added/Changed/Deprecated/Removed/Fixed/Security)")
                .arg(category);
        return r;
    }

    QStringList lines = markdown.split(QLatin1Char('\n'));

    // Locate `## [Unreleased]`.
    int unrel = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed().compare(
                QStringLiteral("## [Unreleased]"), Qt::CaseInsensitive) == 0) {
            unrel = i;
            break;
        }
    }
    if (unrel < 0) {
        r.code = QStringLiteral("not_unreleased");
        r.error = QStringLiteral(
            "changelog_log: no `## [Unreleased]` heading found — the "
            "CHANGELOG must follow Keep-a-Changelog with an Unreleased "
            "section at the top");
        return r;
    }

    // Insert at the TOP of the section (newest-first): right after the
    // `## [Unreleased]` heading and its single blank spacer (if present).
    int insertAt = unrel + 1;
    if (insertAt < lines.size() && lines.at(insertAt).trimmed().isEmpty())
        ++insertAt;

    // Build the dated subsection block.
    QStringList block;
    block.append(QStringLiteral("### %1 %2 — %3")
                     .arg(date.trimmed(), category, headline.trimmed()));
    block.append(QString());                       // blank after the heading
    if (!body.trimmed().isEmpty()) {
        const QStringList bodyLines = body.split(QLatin1Char('\n'));
        for (const QString &ln : bodyLines) block.append(ln);
        block.append(QString());                   // blank after the prose
    }
    for (const QString &bb : bulletBlocks) {
        block += bb.split(QLatin1Char('\n'));
        block.append(QString());                   // blank after each bullet block
    }

    // Repair a missing spacer: if the line the block lands after is not blank
    // (no blank followed `## [Unreleased]`), prepend one so the heading and the
    // new subsection never abut.
    const bool prependedBlank =
        insertAt > 0 && !lines.at(insertAt - 1).trimmed().isEmpty();
    if (prependedBlank) block.prepend(QString());

    for (int k = 0; k < block.size(); ++k)
        lines.insert(insertAt + k, block.at(k));

    r.ok = true;
    r.markdown = lines.join(QLatin1Char('\n'));
    r.line = insertAt + 1 + (prependedBlank ? 1 : 0);   // 1-based heading line
    return r;
}

namespace {
// Case-insensitive canonical index for a `### ` heading name. Returns
// the 0-based Keep-a-Changelog order, or -1 for a non-canonical heading.
// (canonicalCategories().indexOf is case-sensitive; the insert path can
// afford that because it only orders around a matched target, but the
// reorder must place a differently-cased `### fixed` correctly.)
int canonicalIndexCI(const QString &name) {
    const QStringList &cats = canonicalCategories();
    for (int c = 0; c < cats.size(); ++c)
        if (cats.at(c).compare(name, Qt::CaseInsensitive) == 0) return c;
    return -1;
}
}  // namespace

NormalizeResult normalizeUnreleased(const QString &markdown) {
    NormalizeResult r;
    QStringList lines = markdown.split(QLatin1Char('\n'));

    // 1. Locate `## [Unreleased]`.
    int unrel = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed().compare(
                QStringLiteral("## [Unreleased]"), Qt::CaseInsensitive) == 0) {
            unrel = i;
            break;
        }
    }
    if (unrel < 0) {
        r.code = QStringLiteral("not_unreleased");
        r.error = QStringLiteral(
            "changelog_log: no `## [Unreleased]` heading found — the "
            "CHANGELOG must follow Keep-a-Changelog with an Unreleased "
            "section at the top");
        return r;
    }

    // 2. Bound the Unreleased section [unrel+1, sectionEnd).
    int sectionEnd = lines.size();
    for (int i = unrel + 1; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QStringLiteral("## "))) {
            sectionEnd = i;
            break;
        }
    }

    // 3. Refuse a FEATURE-GROUPED section (dated `### ` topics, not flat
    //    categories) — reordering dated topics by category is meaningless
    //    (parity with insertUnreleasedEntry's refusal).
    const int topicLine =
        firstFeatureGroupedTopicLine(lines, unrel + 1, sectionEnd);
    if (topicLine > 0) {
        r.code = QStringLiteral("feature_grouped_section");
        r.error = QStringLiteral(
            "changelog_log: `## [Unreleased]` is feature-grouped — its "
            "`### ` subsections are dated topics (first at line %1), not "
            "Keep-a-Changelog categories; op:normalize only reorders flat "
            "`### <category>` blocks.").arg(topicLine);
        return r;
    }

    // 4. Find the first `### ` heading. Everything from [unrel+1, firstCat)
    //    is preamble (kept untouched); no heading ⇒ nothing to reorder.
    int firstCat = -1;
    for (int i = unrel + 1; i < sectionEnd; ++i) {
        if (lines.at(i).trimmed().startsWith(QStringLiteral("### "))) {
            firstCat = i;
            break;
        }
    }
    if (firstCat < 0) {
        r.ok = true;
        r.markdown = markdown;
        r.changed = false;
        return r;
    }

    // 5. Partition [firstCat, sectionEnd) into `### `-led blocks. A block
    //    spans its heading line through the line before the next `### `
    //    (or sectionEnd) — so its bullets, blank spacer, and any wedged
    //    prose travel with it.
    struct Block { int key; int origin; QStringList body; };
    QVector<Block> blocks;
    int i = firstCat;
    while (i < sectionEnd) {
        const QString name = lines.at(i).trimmed().mid(4).trimmed();
        int j = i + 1;
        while (j < sectionEnd &&
               !lines.at(j).trimmed().startsWith(QStringLiteral("### ")))
            ++j;
        Block b;
        const int ci = canonicalIndexCI(name);
        b.key = (ci >= 0) ? ci : std::numeric_limits<int>::max();
        b.origin = blocks.size();
        for (int k = i; k < j; ++k) b.body.append(lines.at(k));
        blocks.append(b);
        r.order_before.append(name);
        i = j;
    }

    // 6. Stable-sort by canonical key: equal-key (duplicate) and
    //    unknown-category blocks keep their original relative order.
    QVector<Block> sorted = blocks;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Block &a, const Block &b) { return a.key < b.key; });

    for (const Block &b : sorted)
        r.order_after.append(r.order_before.at(b.origin));

    bool changed = false;
    for (int b = 0; b < sorted.size(); ++b)
        if (sorted.at(b).origin != b) { changed = true; break; }

    if (!changed) {
        r.ok = true;
        r.markdown = markdown;
        r.changed = false;
        const int pl = firstInterleavedProseLine(lines, unrel + 1, sectionEnd);
        if (pl > 0) { r.malformed_section = true; r.malformed_line = pl; }
        return r;
    }

    // 7. Reassemble: [0, firstCat) + reordered blocks + [sectionEnd, end).
    //    The result is a permutation of the same lines, so total line
    //    count — and thus the sectionEnd index — is preserved.
    QStringList out;
    for (int k = 0; k < firstCat; ++k) out.append(lines.at(k));
    for (const Block &b : sorted) out += b.body;
    for (int k = sectionEnd; k < lines.size(); ++k) out.append(lines.at(k));

    r.ok = true;
    r.markdown = out.join(QLatin1Char('\n'));
    r.changed = true;
    const int pl = firstInterleavedProseLine(out, unrel + 1, sectionEnd);
    if (pl > 0) { r.malformed_section = true; r.malformed_line = pl; }
    return r;
}

}  // namespace ChangelogLog
