// Feature-conformance test for ANTS-1433 — atomic-write rollback seam
// for cmdRoadmapLog's two-stage QSaveFile commit (ROADMAP.md then
// .roadmap-counter). Spec: tests/features/mcp_roadmap_log_atomicity/spec.md.

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

namespace {

// Minimal ants-v1 roadmap with one real bullet so RoadmapIndex finds
// the "work-items" section and parseBullets is non-empty (dodges the
// unrecognised_format gate). The 📋 is U+1F4CB in UTF-8.
const char *kRoadmap =
    "# Test Roadmap\n"
    "\n"
    "Intro paragraph.\n"
    "\n"
    "## Work Items\n"
    "\n"
    "- \xF0\x9F\x93\x8B [ANTS-0099] **Seed bullet.** Kind: chore.\n";

bool writeFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

QString counterPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral(".roadmap-counter"));
}

// Seed root with ROADMAP.md + .roadmap-counter at the given value.
bool seedProject(const QString &root, const QString &counter) {
    if (!writeFile(roadmapPath(root), QByteArray(kRoadmap))) return false;
    return writeFile(counterPath(root),
                     (counter + QStringLiteral("\n")).toUtf8());
}

QJsonObject appendReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("work-items");
    o[QStringLiteral("status")]     = QStringLiteral("planned");
    o[QStringLiteral("headline")]   =
        QStringLiteral("Atomicity probe bullet.");
    o[QStringLiteral("kind")]       = QStringLiteral("test");
    o[QStringLiteral("source")]     =
        QStringLiteral("ants-1433-regression");
    return o;
}

}  // namespace

// INV-1 — control: the test seam drives the real append path without a
// MainWindow, and a clean run advances both files.
TEST(McpRoadmapLogAtomicity, Inv1HappyPathAppends) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path(), QStringLiteral("100")));

    RemoteControl::setForceCounterCommitFailForTest(false);
    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path())).object();

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "happy-path append should succeed";
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("ANTS-0101"));
    EXPECT_EQ(readFile(counterPath(tmp.path())).trimmed(),
              QByteArray("101"))
        << "counter must advance to the new id on success";
    const QByteArray roadmap = readFile(roadmapPath(tmp.path()));
    EXPECT_TRUE(roadmap.contains("ANTS-0101"))
        << "new bullet must be spliced into ROADMAP.md";
    EXPECT_TRUE(roadmap.contains("Atomicity probe bullet."));
}

// INV-2 — counter integrity: a forced counter-commit failure refuses
// with the documented code and leaves .roadmap-counter untouched.
TEST(McpRoadmapLogAtomicity, Inv2CounterFailureKeepsCounter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path(), QStringLiteral("100")));

    RemoteControl rc(nullptr);
    RemoteControl::setForceCounterCommitFailForTest(true);
    const QJsonObject resp =
        rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path())).object();
    RemoteControl::setForceCounterCommitFailForTest(false);

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("counter_write_failed"));
    EXPECT_EQ(readFile(counterPath(tmp.path())).trimmed(),
              QByteArray("100"))
        << "counter must NOT advance when its commit fails — otherwise "
           "the next allocation reuses the id";
}

// INV-3 — rollback: a forced counter-commit failure restores
// ROADMAP.md to its exact pre-call bytes (all-or-nothing).
TEST(McpRoadmapLogAtomicity, Inv3RoadmapRolledBack) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(seedProject(tmp.path(), QStringLiteral("100")));
    const QByteArray before = readFile(roadmapPath(tmp.path()));

    RemoteControl rc(nullptr);
    RemoteControl::setForceCounterCommitFailForTest(true);
    (void)rc.cmdRoadmapLogAppendForTest(appendReq(tmp.path()));
    RemoteControl::setForceCounterCommitFailForTest(false);

    const QByteArray after = readFile(roadmapPath(tmp.path()));
    EXPECT_FALSE(after.contains("ANTS-0101"))
        << "rolled-back ROADMAP.md must not contain the new id";
    EXPECT_FALSE(after.contains("Atomicity probe bullet."));
    EXPECT_EQ(after, before)
        << "ROADMAP.md must be byte-identical to its pre-call content";
}
