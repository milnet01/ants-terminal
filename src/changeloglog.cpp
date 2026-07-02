// ANTS-1548: see changeloglog.h.

#include "changeloglog.h"

#include <QStringList>

namespace ChangelogLog {

namespace {
// Canonical Keep-a-Changelog category order (spec § sections).
const QStringList &canonicalCategories() {
    static const QStringList v = {
        QStringLiteral("Added"),   QStringLiteral("Changed"),
        QStringLiteral("Deprecated"), QStringLiteral("Removed"),
        QStringLiteral("Fixed"),   QStringLiteral("Security"),
    };
    return v;
}
}  // namespace

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
int firstInterleavedProseLine(const QStringList &lines,
                              int sectionStart, int sectionEnd) {
    bool sawCategory = false;
    for (int i = sectionStart; i < sectionEnd && i < lines.size(); ++i) {
        const QString &raw = lines.at(i);
        const QString t = raw.trimmed();
        if (t.isEmpty()) continue;                  // blank spacer
        if (t.startsWith(QLatin1Char('#'))) {       // any heading line
            if (t.startsWith(QStringLiteral("### "))) sawCategory = true;
            continue;
        }
        if (!sawCategory) continue;                 // pre-category preamble
        // A list item is a `-`/`*`/`+` marker followed by a space; a bare
        // run like `---`/`***` is a thematic break (the stray-footer
        // separator), NOT a bullet, so it must still trip the advisory.
        if (t.size() >= 2 && t.at(1) == QLatin1Char(' ') &&
            (t.at(0) == QLatin1Char('-') || t.at(0) == QLatin1Char('*') ||
             t.at(0) == QLatin1Char('+'))) continue;
        if (raw.startsWith(QLatin1Char(' ')) ||     // indented continuation
            raw.startsWith(QLatin1Char('\t'))) continue;
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

}  // namespace ChangelogLog
