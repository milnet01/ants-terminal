// Shared test helper: render roadmap markdown through the live card
// renderer with every section expanded.
//
// renderCardsHtml emits a section's cards only while its slug is in
// CardRenderOptions::expandedSections, and an h3 only while its h2 is
// open too. A test asserting what the filters, sort or search keep
// therefore opens every section first, so a missing card means the
// filter dropped it, not that its section was collapsed. ANTS-1263
// moved the v1 renderHtml suites onto this.
#pragma once

#include "roadmapdialog.h"
#include "roadmapindex.h"

#include <QSet>
#include <QString>
#include <QStringList>

namespace ants_test {

inline QString cardsAllOpen(
    const QString &markdown, unsigned filter,
    const QStringList &currentBullets = {},
    const QString &theme = QStringLiteral("default"),
    RoadmapDialog::SortOrder sortOrder = RoadmapDialog::SortOrder::Document,
    const QString &search = QString(),
    const QSet<QString> &kindFilter = {},
    RoadmapDialog::Preset preset = RoadmapDialog::Preset::Full) {
    RoadmapDialog::CardRenderOptions opts;
    opts.activePreset = preset;
    for (const auto &section : RoadmapIndex::buildIndex(markdown))
        opts.expandedSections.insert(section.slug);
    return RoadmapDialog::renderCardsHtml(markdown, filter, currentBullets,
                                          theme, sortOrder, search,
                                          kindFilter, opts);
}

}  // namespace ants_test
