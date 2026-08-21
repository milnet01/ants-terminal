// ANTS-4575 — feature-conformance test for `id_inferred`: which roadmap
// ids the reader ADOPTED from a bold prose lead-in rather than read from a
// bracket token. Behavioural against RoadmapParse::parseBullets over
// synthetic ants-v1 and github-task-list fixtures (no real ROADMAP.md),
// plus one source-scrape for the envelope's site coverage (INV-8).
//
// Contract: tests/features/roadmap_query_id_inferred/spec.md

#include "../../_support/srcgrep.h"
#include "remotecontrol.h"   // INV-9 — drives the verb end to end
#include "roadmapparse.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QVector>

#include <string>

namespace {

// An ants-v1 doc. The `[ANTS-0001]` emoji bullet makes the sniffer detect
// ants-v1, so every bullet here takes the native branch.
QString antsV1Doc() {
    return QString::fromUtf8(
        "## Work\n"
        "\n"
        "- \xE2\x9C\x85 [ANTS-0001] **Normal bullet.**\n"
        "- \xF0\x9F\x93\x8B **Cl9.** Short headline here.\n"
        "- \xF0\x9F\x93\x8B [Cb7] **Bracketed headline.**\n"
        "- \xF0\x9F\x9A\xA7 **In-progress thing.** narrator body.\n");
}

// A github-task-list doc — the Vestige shape. The checkbox lines make
// detectRoadmapFormat pick "github-task-list", so the bold lead-in on each
// item is adopted as its id.
QString gfmDoc() {
    return QString::fromUtf8(
        "## Slice 17\n"
        "\n"
        "- [ ] **FW W5** \xE2\x80\x94 Add a reference-regression spec.\n"
        "- [x] **Terrain System** \xE2\x80\x94 Cloth solver landed.\n"
        "- [ ] Plain prose item with no bold span at all.\n"
        "- [ ] **Audit X1** \xE2\x80\x94 see [ANTS-9999] for context.\n");
}

const RoadmapParse::BulletRecord *byHeadlineContains(
        const QVector<RoadmapParse::BulletRecord> &bs, const char *needle) {
    for (const auto &b : bs) {
        if (b.headline.contains(QString::fromUtf8(needle))) return &b;
    }
    return nullptr;
}

}  // namespace

// INV-1 — a GFM bold lead-in reads inferred. The Vestige shape, and the
// reason this feature exists.
TEST(roadmap_query_id_inferred, Inv1GfmBoldLeadInIsInferred) {
    const auto bullets = RoadmapParse::parseBullets(gfmDoc());
    const auto *b = byHeadlineContains(bullets, "reference-regression spec");
    ASSERT_NE(b, nullptr) << "the bold-lead-in bullet must parse";
    EXPECT_EQ(b->id, QStringLiteral("FW W5"))
        << "precondition: the reader adopts the bold span as the id";
    EXPECT_TRUE(RoadmapParse::idWasInferred(*b))
        << "INV-1: an id adopted from a bold prose lead-in must read "
           "inferred";
}

// INV-2 — a declared bracket id does not.
TEST(roadmap_query_id_inferred, Inv2DeclaredBracketIdIsNotInferred) {
    const auto bullets = RoadmapParse::parseBullets(antsV1Doc());
    const auto *b = byHeadlineContains(bullets, "Normal bullet");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->id, QStringLiteral("ANTS-0001"));
    EXPECT_FALSE(RoadmapParse::idWasInferred(*b))
        << "INV-2: `[ANTS-0001]` was written down, not guessed";
}

// INV-3 — the ants-v1 bold-dotted adoption (ANTS-1987) also reads inferred.
// Fails if the implementation guards on `format`.
TEST(roadmap_query_id_inferred, Inv3AntsV1BoldAdoptionIsInferred) {
    const auto bullets = RoadmapParse::parseBullets(antsV1Doc());
    const auto *b = byHeadlineContains(bullets, "Short headline here");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->id, QStringLiteral("Cl9"))
        << "precondition: ANTS-1987 adopts a bold-dotted token natively";
    EXPECT_TRUE(RoadmapParse::idWasInferred(*b))
        << "INV-3: adoption is inference on the native branch too — the "
           "predicate must not be GFM-only";
}

// INV-4 — a head-anchored bare-bracket id does not.
TEST(roadmap_query_id_inferred, Inv4BareBracketIdIsNotInferred) {
    const auto bullets = RoadmapParse::parseBullets(antsV1Doc());
    const auto *b = byHeadlineContains(bullets, "Bracketed headline");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->id, QStringLiteral("Cb7"));
    EXPECT_FALSE(RoadmapParse::idWasInferred(*b))
        << "INV-4: `[Cb7]` is adopted by ANTS-1987 but was written in "
           "brackets, so it is declared";
}

// INV-5 — an id-less bullet is never inferred.
TEST(roadmap_query_id_inferred, Inv5EmptyIdIsNotInferred) {
    const auto bullets = RoadmapParse::parseBullets(antsV1Doc());
    const auto *b = byHeadlineContains(bullets, "In-progress thing");
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(b->id.isEmpty())
        << "precondition: a multi-word bold span is not ID-shaped natively";
    EXPECT_FALSE(RoadmapParse::idWasInferred(*b))
        << "INV-5: an absent id is not a guessed one";
}

// INV-6 — a synthetic content-hash id is not inferred. The two fields
// answer different questions and must never both fire on one bullet.
TEST(roadmap_query_id_inferred, Inv6SyntheticIdIsNotInferred) {
    const auto bullets = RoadmapParse::parseBullets(gfmDoc());
    int synthetic = 0;
    for (const auto &b : bullets) {
        if (!b.synthetic) continue;
        ++synthetic;
        EXPECT_FALSE(RoadmapParse::idWasInferred(b))
            << "INV-6: `synthetic` already says this id was manufactured; "
               "`id_inferred` must not also fire on "
            << b.headline.toStdString();
    }
    EXPECT_GT(synthetic, 0)
        << "precondition: the bold-less GFM item must get a content-hash id";
}

// INV-7 — the predicate follows the ID, not the bullet. A bold lead-in
// whose body cites a [PROJ-NNNN] reports the citation as its id, so it is
// declared. Pins current behaviour so changing it is deliberate.
TEST(roadmap_query_id_inferred, Inv7FollowsTheIdNotTheBullet) {
    const auto bullets = RoadmapParse::parseBullets(gfmDoc());
    const auto *b = byHeadlineContains(bullets, "for context");
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->id, QStringLiteral("ANTS-9999"))
        << "precondition: rxId matches body-wide and wins over the bold "
           "branch";
    EXPECT_EQ(b->boldId, QStringLiteral("Audit X1"))
        << "precondition: the bold span is still recorded";
    EXPECT_FALSE(RoadmapParse::idWasInferred(*b))
        << "INV-7: the reported id came from a bracket token";
}

#if defined(ANTS_RC_SOURCES)
// INV-8 — every roadmap_query bullet-fill site that emits `bold_id` also
// emits `id_inferred`. `bold_id` shipped at three sites and has since grown
// to five; a sixth must not carry one without the other.
TEST(roadmap_query_id_inferred, Inv8EveryFillSiteEmitsTheField) {
    const std::string cpp = ants_test::slurpRemoteControl();
    ASSERT_FALSE(cpp.empty()) << "RemoteControl sources must be readable";
    const int boldSites =
        static_cast<int>(ants_test::countOccurrences(
            cpp, "\"bold_id\"] = b.boldId"));
    const int inferredSites =
        static_cast<int>(ants_test::countOccurrences(
            cpp, "\"id_inferred\"] = true"));
    EXPECT_GT(boldSites, 0) << "precondition: bold_id emission sites found";
    EXPECT_GE(inferredSites, boldSites)
        << "INV-8: " << boldSites << " bold_id fill sites but "
        << inferredSites << " id_inferred sites — a bullet-fill path is "
           "missing the field";
}
#endif

// INV-9 — the flag REACHES THE CALLER. INV-8 proves the emit line exists in
// the source; it cannot prove the field survives into the envelope, and
// ANTS-1881's headline_only projection strips it deliberately, so a
// projection is a real way to lose it. Drives cmdRoadmapQuery end to end
// over a Vestige-shaped roadmap carrying all three id origins at once.
TEST(roadmap_query_id_inferred, Inv9FieldReachesTheEnvelope) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QFile f(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QByteArray seed = "# Roadmap\n\n";
    for (int i = 0; i < 30; ++i)
        seed += "Padding to clear the minimum-parseable-size gate. \n";
    seed +=
        "\n## Systems\n\n"
        // inferred — the id was adopted from the bold lead-in
        "- [ ] **Terrain System** \xE2\x80\x94 heightmap streaming.\n"
        "\n"
        // declared — the author wrote a bracket token
        "- [ ] [ANTS-0001] **Declared bullet.**\n"
        "\n"
        // synthetic — no bold span, no token, so the id is a content hash
        "- [ ] no bold lead-in, so the reader synthesises a hash id.\n"
        "\n";
    ASSERT_EQ(f.write(seed), seed.size());
    f.close();

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = tmp.path();
    r[QStringLiteral("status")]     = QStringLiteral("all");
    const QJsonObject resp = rc.cmdRoadmapQuery(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    const QString dump = QString::fromUtf8(QJsonDocument(bullets).toJson());

    int seenInferred = 0, seenDeclared = 0, seenSynthetic = 0;
    for (const auto &v : bullets) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        const bool has = o.contains(QStringLiteral("id_inferred"));
        if (id == QStringLiteral("Terrain System")) {
            ++seenInferred;
            EXPECT_TRUE(has && o.value(QStringLiteral("id_inferred")).toBool())
                << "INV-9: the adopted id must carry id_inferred:true in the "
                   "envelope, not just in the record\n"
                << dump.toStdString();
        } else if (id == QStringLiteral("ANTS-0001")) {
            ++seenDeclared;
            EXPECT_FALSE(has)
                << "INV-9: a declared id must not carry the key AT ALL — the "
                   "field is gated, not emitted as false";
        } else if (o.value(QStringLiteral("synthetic")).toBool()) {
            ++seenSynthetic;
            EXPECT_FALSE(has)
                << "INV-9: a content-hash id must not carry id_inferred";
        }
    }
    EXPECT_EQ(seenInferred, 1) << "fixture: the bold-lead-in bullet\n"
                               << dump.toStdString();
    EXPECT_EQ(seenDeclared, 1) << "fixture: the bracket-id bullet\n"
                               << dump.toStdString();
    EXPECT_EQ(seenSynthetic, 1) << "fixture: the hash-id bullet\n"
                                << dump.toStdString();
}
