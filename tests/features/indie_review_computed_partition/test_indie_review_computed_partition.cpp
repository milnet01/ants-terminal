// ANTS-3709 — feature-conformance test: IndieReviewEngine::
// deriveComputedPartition builds lanes from the file tree when no module
// map parses, instead of leaving the caller with an empty partition. See
// tests/features/indie_review_computed_partition/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "indiereviewengine.h"
#include "subsystemmap.h"

#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {

void writeFile(const QString &root, const QString &rel, const QByteArray &body) {
    const QString abs = QDir(root).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

// A project that describes its layout in prose — the shape every
// document-reading deriver returns nothing for.
QString seedProse(QTemporaryDir &tmp, const QStringList &relFiles) {
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    writeFile(root, QStringLiteral("CLAUDE.md"),
              "# Proj\n\nThe engine lives alongside the platform layer; "
              "see the README for how they fit together.\n");
    for (const QString &rel : relFiles)
        writeFile(root, rel, QByteArray("int x;\n"));
    SubsystemMap::clearCacheForTests();
    return root;
}

const IndieReviewEngine::Lane *laneNamed(
    const QList<IndieReviewEngine::Lane> &lanes, const QString &name) {
    for (const auto &l : lanes)
        if (l.name == name) return &l;
    return nullptr;
}

}  // namespace

// INV-1 — a prose layout still partitions, one lane per directory.
TEST(IndieReviewComputedPartition, Inv1ProseLayoutYieldsDirectoryLanes) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("linuxdoom-1.10/m_misc.c"),
        QStringLiteral("linuxdoom-1.10/w_wad.c"),
        QStringLiteral("textscreen/txt_button.c"),
    });

    // The document-reading deriver finds nothing to work with...
    EXPECT_TRUE(IndieReviewEngine::derivePartition(root).isEmpty());
    // ...but the tree does.
    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    ASSERT_EQ(lanes.size(), 2) << "one lane per containing directory";
    const auto *doom = laneNamed(lanes, QStringLiteral("linuxdoom-1.10"));
    ASSERT_NE(doom, nullptr);
    EXPECT_EQ(doom->sourcePaths.size(), 2);
    EXPECT_TRUE(doom->sourcePaths.contains(
        QStringLiteral("linuxdoom-1.10/m_misc.c")));
    EXPECT_NE(laneNamed(lanes, QStringLiteral("textscreen")), nullptr);
}

// INV-2 — declared source_roots bound the walk.
TEST(IndieReviewComputedPartition, Inv2DeclaredSourceRootsBoundTheWalk) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("code/engine/a.c"),
        QStringLiteral("code/platform/b.c"),
        QStringLiteral("thirdparty/vendored/z.c"),
    });
    writeFile(root, QStringLiteral(".ants/project.json"),
              "{\"source_roots\":[\"code\"]}\n");

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    ASSERT_EQ(lanes.size(), 2);
    for (const auto &l : lanes)
        for (const QString &p : l.sourcePaths)
            EXPECT_TRUE(p.startsWith(QStringLiteral("code/")))
                << "walk escaped the declared source_roots: "
                << p.toStdString();
}

// INV-3 — a flat directory over the per-lane cap splits into sub-lanes.
TEST(IndieReviewComputedPartition, Inv3LargeDirectorySplitsIntoSubLanes) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QStringList files;
    for (int i = 0; i < 60; ++i)
        files << QStringLiteral("src/f%1.c").arg(i, 2, 10, QLatin1Char('0'));
    const QString root = seedProse(tmp, files);

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    ASSERT_EQ(lanes.size(), 3) << "60 files at a 25-file cap → 3 sub-lanes";
    EXPECT_EQ(lanes[0].name, QStringLiteral("src (1/3)"));
    EXPECT_EQ(lanes[0].sourcePaths.size(), 25);
    EXPECT_EQ(lanes[2].sourcePaths.size(), 10);
    // Deterministic: sorted paths, so the split repeats across derivations.
    EXPECT_EQ(lanes[0].sourcePaths.first(), QStringLiteral("src/f00.c"));
    EXPECT_EQ(lanes[2].sourcePaths.last(), QStringLiteral("src/f59.c"));
}

// INV-4 — one small directory is not a partition; the guard holds.
TEST(IndieReviewComputedPartition, Inv4SingleDirectoryYieldsEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("src/a.c"), QStringLiteral("src/b.c"),
    });
    EXPECT_TRUE(IndieReviewEngine::deriveComputedPartition(root).isEmpty())
        << "a single lane must keep the caller's sparse_partition path";
}

// INV-5 — the handler labels the computed partition and keeps the hint.
TEST(IndieReviewComputedPartition, Inv5HandlerLabelsDerivedAndKeepsHint) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto fn = rc.find("RemoteControl::cmdIndieReviewPartition");
    ASSERT_NE(fn, std::string::npos);
    const std::string body = rc.substr(fn, 4000);

    const auto guard = body.find("if (lanes.size() <= 1)");
    const auto call  = body.find("deriveComputedPartition");
    ASSERT_NE(guard, std::string::npos)
        << "the fallback must be gated — a good partition is never shadowed";
    ASSERT_NE(call, std::string::npos);
    EXPECT_LT(guard, call);
    EXPECT_NE(body.find("env[\"derived\"]"), std::string::npos)
        << "a computed guess must not ship as a declared partition";
    EXPECT_NE(body.find("lanes.size() <= 1 || derived"), std::string::npos)
        << "the sparse hint still applies to a derived partition";
}
