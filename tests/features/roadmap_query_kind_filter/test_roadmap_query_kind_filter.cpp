// ANTS-4836 — roadmap_query `kind` filter. See spec.md.
//
// Drives RemoteControl::cmdRoadmapQuery live against a seeded temp
// ROADMAP.md (the null m_main is never dereferenced on this path), the same
// shape roadmap_query_id_body_cap uses.

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Four bullets across three kinds, so a filter has something to exclude.
QByteArray roadmapWithKinds() {
    return QByteArray(
        "# Roadmap\n\n"
        "## Work\n\n"
        "- \xF0\x9F\x93\x8B [ANTS-9001] **A review fix.**\n"
        "  Kind: review-fix.\n"
        "  Source: test.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9002] **A plain fix.**\n"
        "  Kind: fix.\n"
        "  Source: test.\n"
        "- \xF0\x9F\x93\x8B [ANTS-9003] **Another review fix.**\n"
        "  Kind: review-fix.\n"
        "  Source: test.\n"
        "- \xE2\x9C\x85 [ANTS-9004] **A shipped review fix.**\n"
        "  Kind: review-fix.\n"
        "  Source: test.\n");
}

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QStringList idsOf(const QJsonObject &resp) {
    QStringList out;
    for (const auto &v : resp.value(QStringLiteral("bullets")).toArray())
        out << v.toObject().value(QStringLiteral("id")).toString();
    return out;
}

QJsonObject queryWith(const QString &root, const QJsonObject &extra) {
    RemoteControl rc(nullptr);
    QJsonObject req = extra;
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdRoadmapQuery(req).object();
}

}  // namespace

// INV-1 — kind narrows the list, and composes with status.
TEST(RoadmapQueryKindFilter, Inv1NarrowsAndComposesWithStatus) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          roadmapWithKinds()));

    QJsonObject a;
    a[QStringLiteral("kind")] = QStringLiteral("review-fix");
    const QJsonObject all = queryWith(tmp.path(), a);
    ASSERT_TRUE(all.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(all).toJson().toStdString();
    const QStringList allIds = idsOf(all);
    EXPECT_EQ(allIds.size(), 3)
        << "three bullets carry Kind: review-fix — got "
        << allIds.join(QStringLiteral(",")).toStdString();
    EXPECT_FALSE(allIds.contains(QStringLiteral("ANTS-9002")))
        << "the plain fix must be excluded";

    // The question the reporter could not ask: which review fixes are OPEN.
    QJsonObject b;
    b[QStringLiteral("kind")]   = QStringLiteral("review-fix");
    b[QStringLiteral("status")] = QStringLiteral("active");
    const QStringList open = idsOf(queryWith(tmp.path(), b));
    EXPECT_EQ(open.size(), 2)
        << "the shipped one must drop out: "
        << open.join(QStringLiteral(",")).toStdString();
    EXPECT_FALSE(open.contains(QStringLiteral("ANTS-9004")));
}

// INV-2 — an unrecognised kind refuses, and names the vocabulary.
TEST(RoadmapQueryKindFilter, Inv2UnknownKindRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          roadmapWithKinds()));

    QJsonObject a;
    a[QStringLiteral("kind")] = QStringLiteral("reviewfix");
    const QJsonObject env = queryWith(tmp.path(), a);
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool())
        << "a silently ignored filter returns the FULL set, which reads as "
           "'nothing matches that kind'";
    EXPECT_EQ(env.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_kind"));
    const QJsonArray accepted = env.value(QStringLiteral("accepted")).toArray();
    EXPECT_FALSE(accepted.isEmpty())
        << "mirror bad_status: name the vocabulary so the caller self-corrects";
    bool sawReviewFix = false;
    for (const auto &v : accepted)
        if (v.toString() == QStringLiteral("review-fix")) sawReviewFix = true;
    EXPECT_TRUE(sawReviewFix);
}

// INV-3 — the filter survives the lean projection, which drops the field.
TEST(RoadmapQueryKindFilter, Inv3AppliesUnderHeadlineOnly) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          roadmapWithKinds()));

    QJsonObject a;
    a[QStringLiteral("kind")] = QStringLiteral("review-fix");
    a[QStringLiteral("mode")] = QStringLiteral("headline_only");
    const QJsonObject env = queryWith(tmp.path(), a);
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    const QJsonArray rows = env.value(QStringLiteral("bullets")).toArray();
    EXPECT_EQ(rows.size(), 3);
    // ANTS-4699 — the mode's contract is four keys; kind is not among them.
    EXPECT_FALSE(rows.at(0).toObject().contains(QStringLiteral("kind")))
        << "the lean mode must keep its key set; the FILTER is what applies";
}

// INV-4 — absent means unfiltered, and the echo only appears when applied.
TEST(RoadmapQueryKindFilter, Inv4AbsentIsUnfiltered) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")),
                          roadmapWithKinds()));

    const QJsonObject env = queryWith(tmp.path(), QJsonObject{});
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(idsOf(env).size(), 4);
    EXPECT_FALSE(env.contains(QStringLiteral("kind")))
        << "no filter applied, so nothing to echo";
}
