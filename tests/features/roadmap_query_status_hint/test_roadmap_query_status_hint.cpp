// ANTS-5351 — roadmap_query's bad_status hints at `active` for a list value.
// Contract: spec.md here.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
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
