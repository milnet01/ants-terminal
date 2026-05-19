// ANTS-1287: heading-index engine for ROADMAP.md slice queries.
// Pure (Qt6::Core-only); lives in ants_core_lib. See
// docs/specs/ANTS-1287.md.

#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace RoadmapIndex {

struct Section {
    QString slug;
    QString headingText;
    int level     = 0;   // 2 for `##`, 3 for `###`
    int lineStart = -1;  // 0-indexed line of the heading itself
    int lineEnd   = -1;  // 0-indexed, exclusive end of the section
};

// Walk every line of `markdown`; emit one Section per `##` / `###`
// heading. H1 and H4 are not indexed (INV-3 — parity with parseBullets,
// which only tracks H2/H3 in currentSectionSlug). lineEnd[i] is the
// first subsequent heading line with level <= level[i], or
// total_lines for the last section.
QVector<Section> buildIndex(const QString &markdown);

// Linear lookup by slug. Returns nullptr if no match.
const Section *findBySlug(const QVector<Section> &index, const QString &slug);

// Returns the substring covering lines [section.lineStart,
// section.lineEnd), joined with '\n'. First line is the heading.
QString sliceSection(const QString &markdown, const Section &section);

// Canonical home for the slug helpers — moved out of
// roadmapdialog.cpp so the index and parseBullets share a single
// implementation (INV-4).
int     headingLevel(const QString &raw, QString *text = nullptr);
QString slugifyHeading(const QString &heading);
QString uniqueSlug(QSet<QString> &seen, const QString &heading);

// ANTS-1442 — per-section tally used by roadmap_query's section_index
// mode. Same shape as the `{active,shipped,total}` triplet the verb
// emits.
//
// ANTS-1622 — `*WithId` parallels mirror the default bullets[] filter
// predicate: a bullet whose `id` is empty (narrator prose, rollup, or
// legacy GFM-task-list line without an [PROJ-NNNN] token) doesn't
// contribute. Section_index emits both shapes side-by-side so callers
// reading `active_count` AND `active_count_id_only` see at-a-glance
// when a section's actionable bullets would survive the default
// bullets[] predicate vs when they wouldn't (the legacy-format
// disagreement case the bug report hit).
struct SectionCounts {
    int active        = 0;
    int shipped       = 0;
    int total         = 0;
    int activeWithId  = 0;
    int shippedWithId = 0;
    int totalWithId   = 0;
};

// ANTS-1442 — given direct per-slug tallies (bullets keyed by their
// immediate containing heading slug) and the section index, return a
// tally per slug where each entry sums self + descendant sections.
// `direct` carries only sections whose immediate bullets contributed
// at least one count; the returned hash includes every section in
// `index` (level-2 parents pick up their level-3 children even when
// the level-2 heading has no direct bullets). Pure / Qt-Core-only;
// O(n²) over section count, which is fine at the ~150-section scale
// the live roadmap has reached.
QHash<QString, SectionCounts> rollupCounts(
    const QVector<Section> &index,
    const QHash<QString, SectionCounts> &direct);

}  // namespace RoadmapIndex
