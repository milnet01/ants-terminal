// Feature-conformance test for ANTS-4819 — a section= read must cover the
// heading's descendants, because mode:"section_index" tallies them via the
// descendant-aware RoadmapIndex::rollupCounts. The markdown backend gets
// this from sliceSection's span; the store backend filters records by slug
// and needs the slug SET. INV-1..INV-4 drive descendantSlugs directly;
// INV-5 locks the two backends against each other. See spec.md.

#include "../../_support/expect.h"
#include "roadmapindex.h"
#include "roadmapparse.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

// A level-2 parent with two level-3 children, then a following level-2 so
// INV-3 has a sibling to exclude. Every bullet is ants-v1 shaped so
// parseBullets yields records with the same sectionSlug the index assigns.
const char *kDoc =
    "## Parent sweep\n"
    "Narrative above the children.\n"
    "\n"
    "### Child alpha\n"
    "- 📋 [ANTS-0001] **First child item.**\n"
    "  Kind: fix.\n"
    "\n"
    "### Child beta\n"
    "- ✅ [ANTS-0002] **Second child item.**\n"
    "  Kind: fix.\n"
    "\n"
    "## Following sweep\n"
    "- 📋 [ANTS-0003] **Sibling item.**\n"
    "  Kind: fix.\n";

QSet<QString> slugSet(const QStringList &l) {
    return QSet<QString>(l.begin(), l.end());
}

const RoadmapIndex::Section *bySlug(const QVector<RoadmapIndex::Section> &ix,
                                    const char *slug) {
    return RoadmapIndex::findBySlug(ix, QString::fromUtf8(slug));
}

}  // namespace

// INV-1 / INV-2 / INV-3 — a parent's set is itself plus its children, and
// stops at the sibling that follows it.
TEST(roadmap_query_section_descendants, Inv123ParentCoversChildrenNotSiblings) {
    expect_reset();
    const auto ix = RoadmapIndex::buildIndex(QString::fromUtf8(kDoc));
    const auto *parent = bySlug(ix, "parent-sweep");
    ASSERT_NE(parent, nullptr) << "parent section missing from index";

    const auto got = slugSet(RoadmapIndex::descendantSlugs(ix, *parent));
    expect(got.contains(QStringLiteral("parent-sweep")),
           "INV-1: the section's own slug is in the set");
    expect(got.contains(QStringLiteral("child-alpha")),
           "INV-2: first nested section is in its ancestor's set");
    expect(got.contains(QStringLiteral("child-beta")),
           "INV-2: second nested section is in its ancestor's set");
    expect(!got.contains(QStringLiteral("following-sweep")),
           "INV-3: a section that merely follows is NOT in the set");
    expect(got.size() == 3,
           "INV-3: the set is exactly self + the two children");
    EXPECT_EQ(0, expect_finish());
}

// INV-4 — a leaf returns its own slug alone.
TEST(roadmap_query_section_descendants, Inv4LeafReturnsOnlyItself) {
    expect_reset();
    const auto ix = RoadmapIndex::buildIndex(QString::fromUtf8(kDoc));
    const auto *leaf = bySlug(ix, "child-alpha");
    ASSERT_NE(leaf, nullptr) << "leaf section missing from index";

    const auto got = RoadmapIndex::descendantSlugs(ix, *leaf);
    expect(got.size() == 1 && got.first() == QStringLiteral("child-alpha"),
           "INV-4: a section containing no other returns its own slug alone");
    EXPECT_EQ(0, expect_finish());
}

// INV-5 — backend parity. The markdown backend parses the parent's SLICE;
// the store backend filters whole-project records by slug. Both must cover
// the same section slugs, which is precisely what the defect broke: the
// slice yielded the children's bullets and the slug filter yielded none.
TEST(roadmap_query_section_descendants, Inv5BackendParity) {
    expect_reset();
    const QString doc = QString::fromUtf8(kDoc);
    const auto ix = RoadmapIndex::buildIndex(doc);
    const auto *parent = bySlug(ix, "parent-sweep");
    ASSERT_NE(parent, nullptr) << "parent section missing from index";

    // Markdown path: parse the slice, exactly as the section= branch does.
    const auto sliceBullets = RoadmapParse::parseBullets(
        RoadmapIndex::sliceSection(doc, *parent));
    QSet<QString> fromSlice;
    for (const auto &b : sliceBullets)
        if (!b.id.isEmpty()) fromSlice.insert(b.id);

    // Store path: filter the whole-project records by the slug set.
    const auto whole = RoadmapParse::parseBullets(doc);
    const auto slugs = slugSet(RoadmapIndex::descendantSlugs(ix, *parent));
    QSet<QString> fromStore;
    for (const auto &b : whole)
        if (!b.id.isEmpty() && slugs.contains(b.sectionSlug))
            fromStore.insert(b.id);

    expect(!fromSlice.isEmpty(),
           "INV-5 precondition: the parent's slice yields the children's "
           "bullets, so there is something for the store filter to match");
    expect(fromStore == fromSlice,
           "INV-5: the slug filter admits exactly the ids the markdown "
           "slice yields");
    expect(!fromStore.contains(QStringLiteral("ANTS-0003")),
           "INV-5: the following section's bullet is in neither");
    EXPECT_EQ(0, expect_finish());
}
