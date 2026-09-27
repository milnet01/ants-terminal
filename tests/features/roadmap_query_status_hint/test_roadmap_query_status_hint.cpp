// ANTS-5351 — roadmap_query's bad_status hints at `active` for a list value.
// Contract: spec.md here.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

QJsonObject query(const QTemporaryDir &tmp, const QString &status) {
    QFile f(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")));
    if (f.open(QIODevice::WriteOnly))
        f.write("# Roadmap\n\n## Now\n\n- [ ] **DEMO-0001** A thing.\n");
    f.close();
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("status")]     = status;
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapQueryForTest(req).object();
}

}  // namespace

// INV-1
TEST(RoadmapQueryStatusHint, Inv1ListValueGetsTheHint) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QJsonObject r = query(tmp, QStringLiteral("planned,in-progress"));
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("bad_status"));
    const QString hint = r.value(QStringLiteral("hint")).toString();
    EXPECT_TRUE(hint.contains(QStringLiteral("\"active\"")))
        << QJsonDocument(r).toJson().toStdString();
    EXPECT_TRUE(hint.contains(QStringLiteral("\"all\""))) << hint.toStdString();
}

// INV-2
TEST(RoadmapQueryStatusHint, Inv2SingleUnknownValueHasNoHint) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QJsonObject r = query(tmp, QStringLiteral("bogus"));
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("bad_status"));
    EXPECT_FALSE(r.contains(QStringLiteral("hint")))
        << QJsonDocument(r).toJson().toStdString();
}

// ---------------------------------------------------------------------------
// ANTS-5376 — an ARRAY of statuses is their union.

namespace {

QJsonObject queryArray(const QTemporaryDir &tmp, const QJsonArray &statuses) {
    QFile f(QDir(tmp.path()).filePath(QStringLiteral("ROADMAP.md")));
    if (f.open(QIODevice::WriteOnly))
        f.write("# Roadmap\n\n## Now\n\n"
                "- \xF0\x9F\x93\x8B [ANTS-0001] **Planned.**\n  Kind: feature.\n  Source: test.\n\n"
                "- \xF0\x9F\x9A\xA7 [ANTS-0002] **In progress.**\n  Kind: feature.\n  Source: test.\n\n"
                "- \xE2\x9C\x85 [ANTS-0003] **Shipped.**\n  Kind: feature.\n  Source: test.\n\n"
                "- \xF0\x9F\x92\xAD [ANTS-0004] **Considered.**\n  Kind: feature.\n  Source: test.\n");
    f.close();
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("status")]     = statuses;
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapQueryForTest(req).object();
}

QStringList idsOf(const QJsonObject &r) {
    QStringList ids;
    for (const auto &v : r.value(QStringLiteral("bullets")).toArray())
        ids << v.toObject().value(QStringLiteral("id")).toString();
    ids.sort();
    return ids;
}

}  // namespace

// INV-3
TEST(RoadmapQueryStatusHint, Inv3ArrayIsTheUnion) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QJsonObject r = queryArray(
        tmp, QJsonArray{QStringLiteral("planned"), QStringLiteral("Considered")});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(r).toJson().toStdString();
    EXPECT_EQ(idsOf(r), (QStringList{QStringLiteral("ANTS-0001"),
                                     QStringLiteral("ANTS-0004")}));
    EXPECT_EQ(r.value(QStringLiteral("filter")).toArray(),
              (QJsonArray{QStringLiteral("planned"), QStringLiteral("considered")}))
        << "the echo names the set applied, lower-cased";

    // An aggregate inside the array expands as it does alone.
    const QJsonObject r2 = queryArray(
        tmp, QJsonArray{QStringLiteral("active"), QStringLiteral("shipped")});
    ASSERT_TRUE(r2.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(idsOf(r2), (QStringList{QStringLiteral("ANTS-0001"),
                                      QStringLiteral("ANTS-0002"),
                                      QStringLiteral("ANTS-0003")}));
}

// INV-4
TEST(RoadmapQueryStatusHint, Inv4BadArrayRefuses) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QJsonObject bad = queryArray(
        tmp, QJsonArray{QStringLiteral("planned"), QStringLiteral("bogus")});
    EXPECT_EQ(bad.value(QStringLiteral("code")).toString(), QStringLiteral("bad_status"));
    EXPECT_TRUE(bad.value(QStringLiteral("error")).toString().contains(QStringLiteral("bogus")))
        << "the refusal names the element it could not read";
    const QJsonObject empty = queryArray(tmp, QJsonArray{});
    EXPECT_EQ(empty.value(QStringLiteral("code")).toString(), QStringLiteral("bad_status"))
        << "an empty set would otherwise read as \"all\"";
}

// INV-5
TEST(RoadmapQueryStatusHint, Inv5ListHintNamesTheArrayForm) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString hint = query(tmp, QStringLiteral("planned,considered"))
                             .value(QStringLiteral("hint")).toString();
    EXPECT_TRUE(hint.contains(QStringLiteral("array"))) << hint.toStdString();
}

// INV-6 — a granular status filters identically with and without `section`.
// ANTS-3408's defect was a filter that reached one branch only.
TEST(RoadmapQueryStatusHint, Inv6GranularFilterOnBothPaths) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QJsonObject whole = queryArray(tmp, QJsonArray{QStringLiteral("considered")});
    ASSERT_TRUE(whole.value(QStringLiteral("ok")).toBool());
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = tmp.path();
    req[QStringLiteral("status")]     = QStringLiteral("considered");
    req[QStringLiteral("section")]    = QStringLiteral("now");
    RemoteControl rc(nullptr);
    const QJsonObject sec = rc.cmdRoadmapQueryForTest(req).object();
    ASSERT_TRUE(sec.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(sec).toJson().toStdString();
    EXPECT_EQ(idsOf(whole), QStringList{QStringLiteral("ANTS-0004")});
    EXPECT_EQ(idsOf(sec), idsOf(whole));
}
