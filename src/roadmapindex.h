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

}  // namespace RoadmapIndex
