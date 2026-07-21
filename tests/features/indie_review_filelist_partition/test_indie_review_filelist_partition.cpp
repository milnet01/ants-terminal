// ANTS-3507 — feature-conformance test: IndieReviewEngine::derivePartition
// falls back to grouping a `- <path> — <description>` file-list `## Module
// map` into lanes by top-level directory when the subsystem-shape parse
// (SubsystemMap) derives nothing. Before the fix, such a map yielded an empty
// partition and the caller refused module_map_unparseable. See
// tests/features/indie_review_filelist_partition/spec.md.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "indiereviewengine.h"
#include "subsystemmap.h"

namespace {

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

// Seed a project with a CLAUDE.md + a set of relative files, clearing the
// SubsystemMap cache so each fixture parses fresh.
QString seed(QTemporaryDir &tmp, const QByteArray &claudeMd,
             const QStringList &relFiles) {
    writeFile(tmp.path(), QStringLiteral("CLAUDE.md"), claudeMd);
    for (const QString &rel : relFiles)
        writeFile(tmp.path(), rel, QByteArray("stub\n"));
    SubsystemMap::clearCacheForTests();
    return tmp.path();
}

const IndieReviewEngine::Lane *laneNamed(
    const QList<IndieReviewEngine::Lane> &lanes, const QString &name) {
    for (const auto &l : lanes)
        if (l.name == name) return &l;
    return nullptr;
}

}  // namespace

// INV-1 — a two-top-dir file list yields one lane per top-level directory.
TEST(IndieReviewFileListPartition, Inv1TwoTopDirsOneLaneEach) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray claudeMd =
        "# Proj\n\n## Module map\n\n"
        "- `app/main.py` \xe2\x80\x94 entry point\n"
        "- `app/core/engine.py` \xe2\x80\x94 core engine\n"
        "- `tests/test_engine.py` \xe2\x80\x94 engine tests\n";
    seed(tmp, claudeMd,
         QStringList{QStringLiteral("app/main.py"),
                     QStringLiteral("app/core/engine.py"),
                     QStringLiteral("tests/test_engine.py")});

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    ASSERT_EQ(lanes.size(), 2)
        << "a file-list map under two top dirs must derive two lanes";
    // QMap ordering → alphabetical: app before tests.
    EXPECT_EQ(lanes[0].name, QStringLiteral("app"));
    EXPECT_EQ(lanes[1].name, QStringLiteral("tests"));

    const auto *app = laneNamed(lanes, QStringLiteral("app"));
    ASSERT_NE(app, nullptr);
    EXPECT_TRUE(app->sourcePaths.contains(QStringLiteral("app/main.py")));
    EXPECT_TRUE(app->sourcePaths.contains(QStringLiteral("app/core/engine.py")));

    const auto *tests = laneNamed(lanes, QStringLiteral("tests"));
    ASSERT_NE(tests, nullptr);
    EXPECT_TRUE(tests->sourcePaths.contains(
        QStringLiteral("tests/test_engine.py")));
}

// INV-2 — a file list entirely under one top dir yields an empty partition
// (the >1-lane guard keeps the caller's refusal).
TEST(IndieReviewFileListPartition, Inv2SingleTopDirEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray claudeMd =
        "# Proj\n\n## Module map\n\n"
        "- `app/main.py` \xe2\x80\x94 entry point\n"
        "- `app/core/engine.py` \xe2\x80\x94 core engine\n";
    seed(tmp, claudeMd,
         QStringList{QStringLiteral("app/main.py"),
                     QStringLiteral("app/core/engine.py")});

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    EXPECT_TRUE(lanes.isEmpty())
        << "a single-top-dir file list must not auto-partition (guard)";
}

// INV-3 — a subsystem-shape map still partitions by named subsystem; the
// file-list fallback never engages when the primary parse yields lanes.
TEST(IndieReviewFileListPartition, Inv3SubsystemShapeUnshadowed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray claudeMd =
        "# Proj\n\n## Module map (src/)\n\n"
        "- `foo` \xe2\x80\x94 the foo subsystem\n";
    seed(tmp, claudeMd,
         QStringList{QStringLiteral("src/foo.h"),
                     QStringLiteral("src/foo.cpp")});

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    const auto *foo = laneNamed(lanes, QStringLiteral("foo"));
    ASSERT_NE(foo, nullptr)
        << "the subsystem-shape lane must survive unchanged";
    EXPECT_TRUE(foo->sourcePaths.contains(QStringLiteral("src/foo.cpp")));
    // The fallback (which would name a lane after the top dir `src`) must not
    // have engaged.
    EXPECT_EQ(laneNamed(lanes, QStringLiteral("src")), nullptr);
}

// INV-4 — a listed path that does not exist on disk is dropped from its lane.
TEST(IndieReviewFileListPartition, Inv4NonExistentPathDropped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray claudeMd =
        "# Proj\n\n## Module map\n\n"
        "- `app/real.py` \xe2\x80\x94 real file\n"
        "- `app/ghost.py` \xe2\x80\x94 missing file\n"
        "- `tests/t.py` \xe2\x80\x94 a test\n";
    // Note: app/ghost.py is deliberately NOT created.
    seed(tmp, claudeMd,
         QStringList{QStringLiteral("app/real.py"),
                     QStringLiteral("tests/t.py")});

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    ASSERT_EQ(lanes.size(), 2);
    const auto *app = laneNamed(lanes, QStringLiteral("app"));
    ASSERT_NE(app, nullptr);
    EXPECT_TRUE(app->sourcePaths.contains(QStringLiteral("app/real.py")));
    EXPECT_FALSE(app->sourcePaths.contains(QStringLiteral("app/ghost.py")))
        << "a non-existent listed path must not enter a lane";
}

// INV-5 — a file-list entry that escapes the project tree is rejected by the
// isInsideProject guard; no `..` lane, no escaping sourcePaths entry.
TEST(IndieReviewFileListPartition, Inv5TraversalRejected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray claudeMd =
        "# Proj\n\n## Module map\n\n"
        "- `app/real.py` \xe2\x80\x94 real file\n"
        "- `../../../../../../etc/passwd` \xe2\x80\x94 traversal attempt\n"
        "- `tests/t.py` \xe2\x80\x94 a test\n";
    seed(tmp, claudeMd,
         QStringList{QStringLiteral("app/real.py"),
                     QStringLiteral("tests/t.py")});

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    ASSERT_EQ(lanes.size(), 2)
        << "only the two in-tree dirs must form lanes";
    EXPECT_EQ(laneNamed(lanes, QStringLiteral("..")), nullptr);
    for (const auto &l : lanes)
        for (const QString &p : l.sourcePaths)
            EXPECT_FALSE(p.contains(QStringLiteral("..")))
                << "no escaping entry may survive: " << p.toStdString();
}
