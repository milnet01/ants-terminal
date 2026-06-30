// Feature-conformance test for ANTS-3397 — greenfield .roadmap-counter
// auto-init for the counter strategy. See
// tests/features/roadmap_log_greenfield_counter/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "remotecontrol.h"

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

// Greenfield: a hand-authored roadmap with parseable bullets but NO ids
// of any kind, and (the test omits) NO .roadmap-counter.
const char *kGreenfieldRoadmap =
    "# My Project Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B **Set up the build.**\n"
    "- \xF0\x9F\x93\x8B **Write the README.**\n";

// A roadmap that already carries counter-style ids (but, in the test,
// no .roadmap-counter) — a real desync that must keep refusing.
const char *kIdBearingRoadmap =
    "# My Project Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [ANTS-9001] **Pre-existing bullet.** Kind: chore.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}
QString counterPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral(".roadmap-counter"));
}

qint64 readCounter(const QString &root) {
    QFile f(counterPath(root));
    if (!f.open(QIODevice::ReadOnly)) return -1;
    return QString::fromUtf8(f.readAll().trimmed()).toLongLong();
}

QJsonObject appendReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("status")]     = QStringLiteral("planned");
    o[QStringLiteral("headline")]   = QStringLiteral("First real item.");
    o[QStringLiteral("kind")]       = QStringLiteral("chore");
    o[QStringLiteral("source")]     = QStringLiteral("ants-3397-test");
    o[QStringLiteral("id_prefix")]  = QStringLiteral("CL");
    return o;
}

QJsonObject batchReq(const QString &root) {
    QJsonObject b1;
    b1[QStringLiteral("headline")] = QStringLiteral("Batch item one.");
    b1[QStringLiteral("status")]   = QStringLiteral("planned");
    b1[QStringLiteral("kind")]     = QStringLiteral("chore");
    b1[QStringLiteral("source")]   = QStringLiteral("ants-3397-test");
    QJsonObject b2;
    b2[QStringLiteral("headline")] = QStringLiteral("Batch item two.");
    b2[QStringLiteral("status")]   = QStringLiteral("planned");
    b2[QStringLiteral("kind")]     = QStringLiteral("chore");
    b2[QStringLiteral("source")]   = QStringLiteral("ants-3397-test");
    QJsonArray bullets;
    bullets.append(b1);
    bullets.append(b2);

    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("op")]         = QStringLiteral("append_batch");
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("bullets")]    = bullets;
    o[QStringLiteral("id_prefix")]  = QStringLiteral("CL");
    return o;
}

}  // namespace

// INV-1 — greenfield single append auto-creates the counter at 0 and
// allocates the first id.
TEST(RoadmapLogGreenfieldCounter, Inv1SingleAppendGreenfieldAutoInit) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kGreenfieldRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path())).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("CL-0001"));
    EXPECT_TRUE(QFile::exists(counterPath(tmp.path())))
        << ".roadmap-counter must be auto-created";
    EXPECT_EQ(readCounter(tmp.path()), 1)
        << "counter must advance to the first allocated id";
}

// INV-2 — greenfield batch append auto-inits and allocates contiguously.
TEST(RoadmapLogGreenfieldCounter, Inv2BatchGreenfieldAutoInit) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kGreenfieldRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendBatchForTest(batchReq(tmp.path())).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(QFile::exists(counterPath(tmp.path())));
    EXPECT_EQ(readCounter(tmp.path()), 2)
        << "two bullets must consume ids 1 and 2";
}

// INV-3 — a roadmap with existing ids but no counter file is a real
// desync and must keep refusing on both paths.
TEST(RoadmapLogGreenfieldCounter, Inv3ExistingIdsNoCounterStillRefuses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(roadmapPath(tmp.path()),
                          QByteArray(kIdBearingRoadmap)));
    ASSERT_FALSE(QFile::exists(counterPath(tmp.path())));

    RemoteControl rc(nullptr);
    const QJsonObject single =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path())).object();
    EXPECT_FALSE(single.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(single.value(QStringLiteral("code")).toString(),
              QStringLiteral("counter_missing"));

    const QJsonObject batch =
        rc.cmdRoadmapLogAppendBatchForTest(batchReq(tmp.path())).object();
    EXPECT_FALSE(batch.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(batch.value(QStringLiteral("code")).toString(),
              QStringLiteral("counter_missing"));

    EXPECT_FALSE(QFile::exists(counterPath(tmp.path())))
        << "a refused desync must NOT create the counter file";
}

// INV-4 — the greenfield discriminator helper is present in source.
TEST(RoadmapLogGreenfieldCounter, Inv4HelperPresent) {
    const std::string src = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    EXPECT_NE(src.find("rlRoadmapHasAnyBulletId"), std::string::npos)
        << "the greenfield-vs-desync discriminator must exist";
}
