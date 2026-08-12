// ANTS-4095 / ANTS-4096 / ANTS-4100 — feature-conformance test. Three
// indie_review_* defects that each returned a plausible, well-formed, wrong
// answer with nothing in the envelope to say so. See
// tests/features/indie_review_resolution_and_coarseness/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "indiereviewengine.h"
#include "subsystemmap.h"

#include <string>

#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {

void writeFile(const QString &root, const QString &rel, const QByteArray &body) {
    const QString abs = QDir(root).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

QString seedRoot(QTemporaryDir &tmp, const QStringList &relFiles) {
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
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

bool citesFile(const QList<IndieReviewEngine::CorroboratedFinding> &found,
               const QString &file, int line) {
    for (const auto &f : found)
        if (f.file == file && f.line == line) return true;
    return false;
}

}  // namespace

// INV-1 — the reported shape: two lanes cite `d_main.c:1049`, the file lives
// at `linuxdoom-1.10/d_main.c`, and the citation must still corroborate.
TEST(IndieReviewResolution, Inv1UniqueBasenameResolves) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp, {
        QStringLiteral("linuxdoom-1.10/d_main.c"),
        QStringLiteral("linuxdoom-1.10/r_vulkan.cpp"),
    });

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("platform-io"),
                   QStringLiteral("- **d_main.c:1049** -- unchecked return"));
    reports.insert(QStringLiteral("playsim"),
                   QStringLiteral("## Findings\n- d_main.c:1049 -- same spot"));

    IndieReviewEngine::CorroborateStats stats;
    const auto found = IndieReviewEngine::corroboratedFindings(
        root, reports, /*minLanes=*/2, &stats);

    // Resolved to the FULL project-relative path, not the cited basename.
    EXPECT_TRUE(citesFile(found, QStringLiteral("linuxdoom-1.10/d_main.c"), 1049))
        << "a unique basename must resolve; findings=" << found.size();
    EXPECT_EQ(stats.citationsResolved, stats.citationsSeen);
    EXPECT_GT(stats.citationsByBasename, 0);
}

// INV-2 — two files share a basename: corroborating them would manufacture
// agreement between lanes that cited different files.
TEST(IndieReviewResolution, Inv2AmbiguousBasenameStaysDropped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp, {
        QStringLiteral("client/main.c"),
        QStringLiteral("server/main.c"),
    });

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane-a"), QStringLiteral("main.c:42 bad"));
    reports.insert(QStringLiteral("lane-b"), QStringLiteral("main.c:42 bad"));

    IndieReviewEngine::CorroborateStats stats;
    const auto found = IndieReviewEngine::corroboratedFindings(
        root, reports, /*minLanes=*/2, &stats);

    EXPECT_TRUE(found.isEmpty())
        << "an ambiguous basename must not resolve to either file";
    EXPECT_GT(stats.citationsSeen, 0);
    EXPECT_EQ(stats.citationsResolved, 0);

    // And the index itself poisons the key rather than keeping first-wins.
    const auto index = IndieReviewEngine::buildBasenameIndex(root);
    ASSERT_TRUE(index.contains(QStringLiteral("main.c")));
    EXPECT_TRUE(index.value(QStringLiteral("main.c")).isEmpty());
}

// INV-3 — the pair that distinguishes a parse failure from an empty result.
// Also pins the de-duplication: `foo.c:1` is matched by BOTH the file:line
// and the bare-file regex pass, and must be counted once.
TEST(IndieReviewResolution, Inv3UnresolvedCitationsAreCounted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp, {QStringLiteral("src/real.c")});

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("lane-a"),
                   QStringLiteral("nowhere.c:1 and nowhere.c:1 again"));

    IndieReviewEngine::CorroborateStats stats;
    const auto found = IndieReviewEngine::corroboratedFindings(
        root, reports, /*minLanes=*/1, &stats);

    EXPECT_TRUE(found.isEmpty());
    EXPECT_EQ(stats.citationsSeen, 1)
        << "one distinct token, matched by two regex passes and repeated once";
    EXPECT_EQ(stats.citationsResolved, 0);

    // The MCP layer must surface that pair, and must not resurrect
    // total_input_bytes as the tell — it is 0 by design on the disk path.
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("citations_seen"), std::string::npos);
    EXPECT_NE(rc.find("citations_resolved"), std::string::npos);
    EXPECT_NE(rc.find("unresolved_citations"), std::string::npos);
}

// INV-4 — the reported shaders/ directory: generated byte-array headers out,
// hand-written shader stages in.
TEST(IndieReviewPartitionFiles, Inv4ShadersInGeneratedHeadersOut) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp, {
        QStringLiteral("shaders/raygen.rgen"),
        QStringLiteral("shaders/shade.comp"),
        QStringLiteral("shaders/post.frag"),
        QStringLiteral("shaders/raygen.spv.h"),
        QStringLiteral("shaders/shade.spv.h"),
        QStringLiteral("shaders/post.spv.h"),
        QStringLiteral("engine/core.cpp"),
        QStringLiteral("engine/moc_core.cpp"),
    });

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    const auto *shaders = laneNamed(lanes, QStringLiteral("shaders"));
    ASSERT_NE(shaders, nullptr) << "shaders/ must yield a lane";

    for (const QString &p : shaders->sourcePaths) {
        EXPECT_FALSE(p.endsWith(QStringLiteral(".spv.h")))
            << "generated byte-array header in a review lane: " << qPrintable(p);
    }
    EXPECT_TRUE(shaders->sourcePaths.contains(QStringLiteral("shaders/shade.comp")));
    EXPECT_TRUE(shaders->sourcePaths.contains(QStringLiteral("shaders/post.frag")));
    EXPECT_TRUE(shaders->sourcePaths.contains(QStringLiteral("shaders/raygen.rgen")));

    // The prefix-shaped generator this replaced still has to be excluded.
    const auto *engine = laneNamed(lanes, QStringLiteral("engine"));
    ASSERT_NE(engine, nullptr);
    EXPECT_FALSE(engine->sourcePaths.contains(QStringLiteral("engine/moc_core.cpp")));
}

// INV-5 — computed-fallback lanes carry one template, so summary similarity
// is meaningless across them and must never produce a merge suggestion.
TEST(IndieReviewPartitionFiles, Inv5ComputedFallbackSuggestsNoMerges) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp, {
        QStringLiteral("ipx/net.c"),
        QStringLiteral("sndserv/snd.c"),
        QStringLiteral("engine/core.c"),
    });

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    ASSERT_GT(lanes.size(), 1);
    for (const auto &l : lanes)
        ASSERT_TRUE(l.summary.contains(QStringLiteral("grouped by directory")))
            << "test assumes the computed template; got: "
            << qPrintable(l.summary);

    EXPECT_TRUE(IndieReviewEngine::suggestedMerges(lanes).isEmpty())
        << "boilerplate summaries must not drive merges";
}

// INV-6 — a lane naming a DIRECTORY is measured, not counted as one path.
TEST(IndieReviewCoarseness, Inv6LaneFileCountExpandsDirectories) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QStringList files;
    for (int i = 0; i < 40; ++i)
        files << QStringLiteral("src/app/mod%1.cpp").arg(i);
    files << QStringLiteral("src/app/mod0.spv.h");   // generated — not counted
    const QString root = seedRoot(tmp, files);

    IndieReviewEngine::Lane lane;
    lane.name        = QStringLiteral("src");
    lane.sourcePaths = { QStringLiteral("src/app") };   // one entry, a directory

    const int count = IndieReviewEngine::laneFileCount(root, lane);
    EXPECT_EQ(count, 40) << "a directory sourcePath must be walked, not counted as 1";
    EXPECT_GT(count, IndieReviewEngine::kMaxReviewableFilesPerLane)
        << "a whole-application lane must trip the threshold";
}

// INV-7 — a real declared lane must never trip it, or the signal gets ignored.
// This project's own largest module-map lane is 14 files.
TEST(IndieReviewCoarseness, Inv7SmallLaneIsNotCoarse) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedRoot(tmp, {
        QStringLiteral("src/vtparser.cpp"),
        QStringLiteral("src/vtparser.h"),
    });

    IndieReviewEngine::Lane lane;
    lane.name        = QStringLiteral("vtparser");
    lane.sourcePaths = { QStringLiteral("src/vtparser.cpp"),
                         QStringLiteral("src/vtparser.h") };

    const int count = IndieReviewEngine::laneFileCount(root, lane);
    EXPECT_EQ(count, 2);
    EXPECT_LE(count, IndieReviewEngine::kMaxReviewableFilesPerLane);

    // The MCP layer emits the measurement and the flag.
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("file_count"), std::string::npos);
    EXPECT_NE(rc.find("too_coarse_lanes"), std::string::npos);
}
