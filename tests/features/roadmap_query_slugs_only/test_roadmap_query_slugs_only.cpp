// Feature-conformance test for spec.md — ANTS-4467.
//
//   INV-1 every slug returned   INV-2 sections absent
//   INV-3 status filter applies INV-4 inert outside section_index
//
// Behavioural: drives RemoteControl::cmdRoadmapQuery against a fixture
// roadmap. A source-scrape would prove the branch exists; the defect being
// closed is about what the caller actually receives.

#include "remotecontrol.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

bool writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

// Three sections. The third holds only shipped work, so the active filter
// must drop it — that is INV-3's discriminator.
QByteArray fixture() {
    return QByteArrayLiteral(
        "<!-- ants-roadmap: 1 -->\n"
        "# Roadmap\n\n"
        "## Alpha section\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0001] **First.**\n"
        "  Kind: fix.\n"
        "  **Layman:** A thing.\n\n"
        "## Beta section\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-0002] **Second.**\n"
        "  Kind: fix.\n"
        "  **Layman:** A thing.\n\n"
        "## Gamma shipped only\n\n"
        "- \xE2\x9C\x85 [ANTS-0003] **Third.**\n"
        "  Kind: fix.\n"
        "  **Layman:** A thing.\n");
}

QJsonObject query(const QString &root, const QJsonObject &extra) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    for (auto it = extra.begin(); it != extra.end(); ++it)
        req[it.key()] = it.value();
    return rc.cmdRoadmapQuery(req).object();
}

QStringList slugsOf(const QJsonObject &resp, const char *key) {
    QStringList out;
    for (const QJsonValue &v : resp.value(QLatin1String(key)).toArray())
        out << (v.isString() ? v.toString()
                             : v.toObject().value(QStringLiteral("slug"))
                                   .toString());
    return out;
}

}  // namespace

TEST(RoadmapQuerySlugsOnly, Ants4467ProjectionReturnsEverySlug) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
                          fixture()));

    // The object form is the reference: whatever it lists, the projection
    // must list, so the two can never disagree about membership.
    const QJsonObject objects =
        query(tmp.path(), {{QStringLiteral("mode"),
                            QStringLiteral("section_index")}});
    ASSERT_TRUE(objects.value(QStringLiteral("ok")).toBool())
        << objects.value(QStringLiteral("error")).toString().toStdString();
    const QStringList reference = slugsOf(objects, "sections");
    ASSERT_EQ(reference.size(), 3)
        << "precondition: the fixture has three sections";

    const QJsonObject flat =
        query(tmp.path(), {{QStringLiteral("mode"),
                            QStringLiteral("section_index")},
                           {QStringLiteral("slugs_only"), true}});
    ASSERT_TRUE(flat.value(QStringLiteral("ok")).toBool());

    // INV-1 — every slug, in the same order.
    EXPECT_EQ(slugsOf(flat, "slugs"), reference)
        << "ANTS-4467: the projection exists so a caller gets ONE key from "
           "EVERY row — a prefix is the failure it replaces";
    EXPECT_TRUE(flat.value(QStringLiteral("slugs_only")).toBool());
    EXPECT_EQ(flat.value(QStringLiteral("total")).toInt(), 3);

    // INV-2 — one shape or the other, never both.
    EXPECT_FALSE(flat.contains(QStringLiteral("sections")))
        << "ANTS-4467: emitting both would leave a caller guessing which to "
           "read, and would re-introduce the payload the projection removes";
}

TEST(RoadmapQuerySlugsOnly, Ants4467ProjectionHonoursTheStatusFilter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
                          fixture()));

    // INV-3 — a projection, not a mode: the same rows drop.
    const QJsonObject activeObjects =
        query(tmp.path(), {{QStringLiteral("mode"),
                            QStringLiteral("section_index")},
                           {QStringLiteral("status"),
                            QStringLiteral("active")}});
    ASSERT_TRUE(activeObjects.value(QStringLiteral("ok")).toBool());
    const QStringList activeReference = slugsOf(activeObjects, "sections");

    const QJsonObject activeFlat =
        query(tmp.path(), {{QStringLiteral("mode"),
                            QStringLiteral("section_index")},
                           {QStringLiteral("status"),
                            QStringLiteral("active")},
                           {QStringLiteral("slugs_only"), true}});
    ASSERT_TRUE(activeFlat.value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(slugsOf(activeFlat, "slugs"), activeReference);
    EXPECT_FALSE(slugsOf(activeFlat, "slugs")
                     .contains(QStringLiteral("gamma-shipped-only")))
        << "the shipped-only section must drop under status:active in BOTH "
           "shapes — a projection that quietly widened the filter would be a "
           "different answer wearing the same name";
}

TEST(RoadmapQuerySlugsOnly, Ants4467IsInertOnTheBulletsPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
                          fixture()));

    // INV-4 — scoped to section_index. A flag that silently reshaped an
    // unrelated mode would be worse than the spill it fixes.
    const QJsonObject plain = query(tmp.path(), {});
    const QJsonObject withFlag =
        query(tmp.path(), {{QStringLiteral("slugs_only"), true}});
    ASSERT_TRUE(plain.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(withFlag.value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(withFlag.value(QStringLiteral("bullets")).toArray().size(),
              plain.value(QStringLiteral("bullets")).toArray().size());
    EXPECT_FALSE(withFlag.contains(QStringLiteral("slugs")));
}

// ANTS-5154 — with_titles pairs each slug with its heading text, so finding
// a section by name needs neither the full rows nor a second call.
TEST(RoadmapQuerySlugsOnly, Ants5154WithTitlesPairsEachSlugWithItsTitle) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"), fixture()));
    const QJsonObject resp =
        query(tmp.path(), {{QStringLiteral("mode"), QStringLiteral("section_index")},
                           {QStringLiteral("slugs_only"), true},
                           {QStringLiteral("with_titles"), true}});
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    const QJsonArray pairs = resp.value(QStringLiteral("slug_titles")).toArray();
    ASSERT_EQ(pairs.size(), 3);
    EXPECT_EQ(pairs.at(0).toObject().value(QStringLiteral("slug")).toString(),
              QStringLiteral("alpha-section"));
    EXPECT_EQ(pairs.at(0).toObject().value(QStringLiteral("headline")).toString(),
              QStringLiteral("Alpha section"));
    EXPECT_EQ(slugsOf(resp, "slugs").size(), 3) << "the flat list stays";

    // Without the flag the reply is unchanged.
    const QJsonObject plain =
        query(tmp.path(), {{QStringLiteral("mode"), QStringLiteral("section_index")},
                           {QStringLiteral("slugs_only"), true}});
    EXPECT_FALSE(plain.contains(QStringLiteral("slug_titles")));
}

// ANTS-4986 — a section's counts include its subsections, so the rows cannot
// be summed. Each row also carries its OWN counts, and the reply says so.
TEST(RoadmapQuerySlugsOnly, Ants4986DirectCountsExcludeSubsections) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
        QByteArrayLiteral(
            "<!-- ants-roadmap: 1 -->\n"
            "# Roadmap\n\n"
            "## Parent\n\n"
            "- \xF0\x9F\x93\x8B [ANTS-0011] **Own item.**\n"
            "  Kind: fix.\n\n"
            "### Child\n\n"
            "- \xF0\x9F\x93\x8B [ANTS-0012] **Child item one.**\n"
            "  Kind: fix.\n"
            "- \xE2\x9C\x85 [ANTS-0013] **Child item two.**\n"
            "  Kind: fix.\n")));
    const QJsonObject resp =
        query(tmp.path(), {{QStringLiteral("mode"), QStringLiteral("section_index")}});
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    QJsonObject parent, child;
    for (const QJsonValue &v : resp.value(QStringLiteral("sections")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("slug")).toString() == QStringLiteral("parent")) parent = o;
        if (o.value(QStringLiteral("slug")).toString() == QStringLiteral("child"))  child = o;
    }
    ASSERT_FALSE(parent.isEmpty());
    ASSERT_FALSE(child.isEmpty());
    EXPECT_EQ(parent.value(QStringLiteral("total_count")).toInt(), 3);
    EXPECT_EQ(parent.value(QStringLiteral("direct_total_count")).toInt(), 1);
    EXPECT_EQ(parent.value(QStringLiteral("direct_active_count")).toInt(), 1);
    EXPECT_EQ(child.value(QStringLiteral("direct_active_count")).toInt(), 1);
    EXPECT_EQ(child.value(QStringLiteral("direct_shipped_count")).toInt(), 1);
    EXPECT_TRUE(resp.value(QStringLiteral("counts_hint")).toString()
                    .contains(QStringLiteral("direct_")))
        << "the double-count must be visible, not merely avoidable";
}
