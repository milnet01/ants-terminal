// Feature-conformance test for ANTS-4819 — a section= read must cover the
// heading's descendants, because mode:"section_index" tallies them via the
// descendant-aware RoadmapIndex::rollupCounts. The markdown backend gets
// this from sliceSection's span; the store backend filters records by slug
// and needs the slug SET. INV-1..INV-4 drive descendantSlugs directly;
// INV-5 locks the two backends against each other. See spec.md.

#include "../../_support/expect.h"
#include "roadmapindex.h"
#include "roadmapparse.h"
#include "remotecontrol.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

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

// INV-6 (ANTS-4824) — a descendant's bullet reports the slug of the section
// it LIVES in, not the one that was queried.
//
// ANTS-1287-INV-7 overwrote every returned bullet's slug with the requested
// one, which was correct while section= returned a section's own bullets:
// the two were the same value, and the overwrite defended against a
// slice-local slug artifact. ANTS-4819 made section= return descendants and
// the two diverged, so the child that made the answer correct became
// invisible in it — and the value stopped being safe to feed back, since
// roadmap_log op:"append" takes a section slug and would file into the
// PARENT without saying so.
//
// Drives the real envelope rather than re-deriving the rule, which would
// only restate the implementation.
TEST(roadmap_query_section_descendants, Inv6DescendantReportsItsOwnSlug) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QFile f(tmp.path() + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(QByteArray("# Roadmap\n\n") + kDoc);
    f.close();

    RemoteControl rc(nullptr);
    QJsonObject req;
    req["caller_cwd"] = tmp.path();
    req["section"]    = QStringLiteral("parent-sweep");
    req["status"]     = QStringLiteral("all");
    const QJsonObject out = rc.cmdRoadmapQuery(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();

    QHash<QString, QString> slugOf;
    const QJsonArray bullets = out.value(QStringLiteral("bullets")).toArray();
    for (const auto &v : bullets) {
        const QJsonObject o = v.toObject();
        slugOf.insert(o.value(QStringLiteral("id")).toString(),
                      o.value(QStringLiteral("section_slug")).toString());
    }

    // Precondition: ANTS-4819's descendant inclusion still holds, so there
    // is a descendant bullet whose slug this invariant is about.
    expect(slugOf.contains(QStringLiteral("ANTS-0001")) &&
           slugOf.contains(QStringLiteral("ANTS-0002")),
           "INV-6 precondition: the parent's read returns both children");
    expect(!slugOf.contains(QStringLiteral("ANTS-0003")),
           "INV-6 precondition: the following section's bullet is excluded");

    expect(slugOf.value(QStringLiteral("ANTS-0001")) ==
               QStringLiteral("child-alpha"),
           "INV-6: a bullet in a nested section reports that section's slug");
    expect(slugOf.value(QStringLiteral("ANTS-0002")) ==
               QStringLiteral("child-beta"),
           "INV-6: each descendant reports its own slug, not one shared one");
    EXPECT_EQ(0, expect_finish());
}
