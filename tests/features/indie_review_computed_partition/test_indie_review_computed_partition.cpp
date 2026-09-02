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
#include <QProcess>
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

// INV-6 — the walk reports what its suffix filter dropped (ANTS-4771).
TEST(IndieReviewComputedPartition, Inv6SuffixFilteredFilesAreReported) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // A mixed tree: Python that partitions, shell that the suffix filter
    // drops. The shell sits in BOTH directories, including alongside the
    // Python at the root of a lane, so a path-shaped explanation is ruled out.
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("oneup/updater.py"),
        QStringLiteral("oneup/bump.py"),
        QStringLiteral("oneup/update_system.sh"),
        QStringLiteral("packaging/build-appimage.sh"),
        QStringLiteral("packaging/release.py"),
    });

    IndieReviewEngine::UnassignedSources dropped;
    const auto lanes = IndieReviewEngine::deriveComputedPartition(root, &dropped);

    // The partition itself is unchanged — this adds reporting, not coverage.
    ASSERT_EQ(lanes.size(), 2);
    const auto *oneup = laneNamed(lanes, QStringLiteral("oneup"));
    ASSERT_NE(oneup, nullptr);
    EXPECT_EQ(oneup->sourcePaths.size(), 2) << "shell is still not a lane member";

    // ...but it is no longer silent about the shell it skipped.
    EXPECT_EQ(dropped.bySuffix.value(QStringLiteral("sh")), 2)
        << "reported per suffix, so a caller sees WHAT was dropped";
    EXPECT_FALSE(dropped.bySuffix.contains(QStringLiteral("py")))
        << "a file that made it into a lane is not unassigned";
    // The seeded CLAUDE.md is dropped by the same filter and is reported too.
    // That is deliberate: it IS a file no lane covers, and deciding that
    // markdown does not count would reintroduce the judgement about which
    // extensions matter that this whole approach exists to avoid. The
    // per-suffix breakdown is what keeps it readable — a caller sees
    // `sh: 2` next to `md: 1` and can tell the signal from the prose.
    EXPECT_EQ(dropped.bySuffix.value(QStringLiteral("md")), 1);
    EXPECT_EQ(dropped.count, 3) << "count is the sum across suffixes";
}

// INV-7 — a caller that passes no reporter still works (ANTS-4771).
TEST(IndieReviewComputedPartition, Inv7NullReporterIsHarmless) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("a/one.py"), QStringLiteral("a/tool.sh"),
        QStringLiteral("b/two.py"),
    });
    // The dialogs call the one-argument form; it must not crash or change.
    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    EXPECT_EQ(lanes.size(), 2);
}

// ANTS-4805 — the CI surface is reachable by the partition walk. isNoiseDir
// drops every dot-directory, which is right for an index and wrong for a
// review partition: .github holds the build and release workflows, and
// dropping it made them invisible to the lanes AND to the coverage report
// that exists to catch that.
TEST(IndieReviewComputedPartition, Ants4805CiSurfaceIsNotSilentlyDropped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("engine/core.py"),
        QStringLiteral("ui/window.py"),
    });
    writeFile(root, QStringLiteral(".github/workflows/ci.yml"),
              QByteArray("name: CI\n"));
    writeFile(root, QStringLiteral(".github/workflows/release.yml"),
              QByteArray("name: Release\n"));
    // A dot-directory that is NOT the CI surface, as the control: the rule is
    // one named exception, not a widening of the dot rule.
    writeFile(root, QStringLiteral(".venv/lib/thing.py"), QByteArray("x = 1\n"));
    SubsystemMap::clearCacheForTests();

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    QStringList sample;
    const auto gap = IndieReviewEngine::unassignedForLanes(root, lanes, &sample);

    // The workflows are YAML, so no lane claims them — but they are now
    // REPORTED as uncovered instead of vanishing before every filter.
    EXPECT_EQ(gap.bySuffix.value(QStringLiteral("yml")), 2)
        << "the CI surface must be visible to the coverage report";
    // .venv is still noise, and its .py would otherwise be indexable — so if
    // the dot rule had been widened rather than excepted, this would fire.
    for (const QString &p : sample)
        EXPECT_FALSE(p.contains(QStringLiteral(".venv")))
            << "the exception must be one directory, not the whole dot rule";
}

// ANTS-4806 — a derived lane over the test tree is LABELLED, not omitted.
// review-code's scope excludes tests and review-tests' is exactly them, so the
// consumer drops what it does not want and the verb decides for neither.
TEST(IndieReviewComputedPartition, Ants4806TestLanesAreLabelled) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("engine/core.py"),
        QStringLiteral("ui/window.py"),
        QStringLiteral("tests/test_core.py"),
    });

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    const auto *tests  = laneNamed(lanes, QStringLiteral("tests"));
    const auto *engine = laneNamed(lanes, QStringLiteral("engine"));
    ASSERT_NE(tests, nullptr) << "the test tree must still BE a lane";
    ASSERT_NE(engine, nullptr);

    EXPECT_EQ(tests->kind, QStringLiteral("tests"));
    EXPECT_TRUE(engine->kind.isEmpty())
        << "an unlabelled lane must stay unlabelled — the absence of a label "
           "is not a claim that the lane is production code";
}

// ANTS-4816 — a lane of files the count cannot admit reports 0 AND says so,
// so "nothing here" is distinguishable from "nothing I could measure".
// finbreak measured three such lanes (.github, assets, packaging) carrying
// real files while counting zero — and 0 is the input too_coarse and
// total_lines both key on.
TEST(IndieReviewComputedPartition, Ants4816UncountedFilesAreReported) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // A packaging lane: real files, none of them a suffix the index outlines.
    writeFile(root, QStringLiteral("packaging/app.spec"), QByteArray("Name: x\n"));
    writeFile(root, QStringLiteral("packaging/debian/control"), QByteArray("Package: x\n"));
    writeFile(root, QStringLiteral("packaging/app.metainfo.xml"), QByteArray("<c/>\n"));
    // And a source lane, as the control.
    writeFile(root, QStringLiteral("src/a.py"), QByteArray("x = 1\n"));

    IndieReviewEngine::Lane pkg;
    pkg.name        = QStringLiteral("packaging");
    pkg.sourcePaths = QStringList{QStringLiteral("packaging")};
    IndieReviewEngine::Lane src;
    src.name        = QStringLiteral("src");
    src.sourcePaths = QStringList{QStringLiteral("src")};

    // The premise: the lane is not empty and counts zero anyway.
    EXPECT_EQ(IndieReviewEngine::laneFileCount(root, pkg), 0);
    EXPECT_EQ(IndieReviewEngine::laneUncountedFiles(root, pkg), 3)
        << "three real files were there and not counted";

    // The control: a lane the walk DOES admit reports nothing uncounted, so
    // the signal means something rather than firing everywhere.
    EXPECT_EQ(IndieReviewEngine::laneFileCount(root, src), 1);
    EXPECT_EQ(IndieReviewEngine::laneUncountedFiles(root, src), 0);

    // A genuinely empty lane is still zero and zero — which is what makes the
    // pair readable as "empty" rather than "unmeasured".
    QDir().mkpath(QDir(root).filePath(QStringLiteral("hollow")));
    IndieReviewEngine::Lane hollow;
    hollow.name        = QStringLiteral("hollow");
    hollow.sourcePaths = QStringList{QStringLiteral("hollow")};
    EXPECT_EQ(IndieReviewEngine::laneFileCount(root, hollow), 0);
    EXPECT_EQ(IndieReviewEngine::laneUncountedFiles(root, hollow), 0);
}

// ANTS-4811 — a project whose sources all sit under ONE directory gets a
// partition. Grouping by directory produced one lane, the >1 gate discarded
// it, and the verb answered `lanes:[], ok:true` — which is what a project with
// no subsystems looks like. LocalWebServerManager hit it with fifteen modules
// under one package directory.
TEST(IndieReviewComputedPartition, Ants4811SingleDirectoryStillPartitions) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // Fifteen modules, one directory, each big enough that the whole is well
    // over a reviewer's budget — the shape file counts cannot see, since
    // fifteen is under every per-lane file threshold.
    QByteArray mod;
    for (int i = 0; i < 400; ++i) mod += "x = " + QByteArray::number(i) + "\n";
    for (int f = 0; f < 15; ++f)
        writeFile(root, QStringLiteral("src/lwsm/m%1.py")
                            .arg(f, 2, 10, QLatin1Char('0')), mod);
    SubsystemMap::clearCacheForTests();

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    ASSERT_GT(lanes.size(), 1)
        << "one directory must still partition, not collapse to nothing";

    // Every file is placed exactly once: a split that drops or duplicates one
    // is worse than the empty partition it replaces.
    QStringList placed;
    for (const auto &l : lanes) placed += l.sourcePaths;
    EXPECT_EQ(placed.size(), 15);
    placed.sort();
    QStringList unique = placed;
    unique.removeDuplicates();
    EXPECT_EQ(unique.size(), 15) << "a file landed in two lanes";

    // Split by SIZE, so each lane is within a reviewer's budget.
    for (const auto &l : lanes)
        EXPECT_LE(IndieReviewEngine::laneLineCount(root, l),
                  IndieReviewEngine::kMaxReviewableLinesPerLane +
                      400 /* one file may overshoot; a file is not divisible */)
            << "lane " << l.name.toStdString();
}

// ANTS-4811 — and a small one-directory project still collapses, because
// there the refusal and its hint are the honest answer rather than a lane per
// handful of lines.
TEST(IndieReviewComputedPartition, Ants4811SmallSingleDirectoryStillCollapses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("src/only/a.py"),
        QStringLiteral("src/only/b.py"),
    });
    EXPECT_TRUE(IndieReviewEngine::deriveComputedPartition(root).isEmpty());
}

// ANTS-4809 — a directory the repository ignores yields no lane, and is not
// reported as a coverage gap either. LottoTracker got 14 of 17 lanes over a
// gitignored tree of scraped HTML; a lane is a subagent told to read what it
// names, and that tree held data the project deliberately excludes.
TEST(IndieReviewComputedPartition, Ants4809GitIgnoredTreeIsNeitherLaneNorGap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    writeFile(root, QStringLiteral("src/engine/core.py"), QByteArray("x = 1\n"));
    writeFile(root, QStringLiteral("src/ui/window.py"), QByteArray("x = 1\n"));
    writeFile(root, QStringLiteral("src/archive_cache/a.py"), QByteArray("x = 1\n"));
    writeFile(root, QStringLiteral("src/archive_cache/b.py"), QByteArray("x = 1\n"));
    writeFile(root, QStringLiteral(".gitignore"),
              QByteArray("src/archive_cache/\n"));

    // A real repository, or check-ignore has nothing to answer from. The
    // helper fails OPEN, so without this the test would pass vacuously —
    // which is why the control assertion below is not optional.
    ASSERT_EQ(QProcess::execute(QStringLiteral("git"),
                                {QStringLiteral("-C"), root,
                                 QStringLiteral("init"), QStringLiteral("-q")}), 0)
        << "git is required for this case; without a repo it proves nothing";

    const auto lanes = IndieReviewEngine::deriveComputedPartition(root);
    ASSERT_FALSE(lanes.isEmpty());
    for (const auto &l : lanes) {
        EXPECT_FALSE(l.name.contains(QStringLiteral("archive_cache")))
            << "a gitignored tree became a review lane: " << l.name.toStdString();
        for (const QString &sp : l.sourcePaths)
            EXPECT_FALSE(sp.contains(QStringLiteral("archive_cache")))
                << "a gitignored file entered a lane: " << sp.toStdString();
    }
    // The control: the walk still found the real source, so the exclusion
    // above is the gitignore rule biting and not an empty partition.
    EXPECT_NE(laneNamed(lanes, QStringLiteral("src/engine")), nullptr);

    // And it is not a coverage gap: an ignored file is not uncovered, so
    // reporting it would be a false gap. The two walks must exclude the same
    // set or they contradict each other about one tree.
    QStringList sample;
    const auto gap = IndieReviewEngine::unassignedForLanes(root, lanes, &sample);
    for (const QString &p : sample)
        EXPECT_FALSE(p.contains(QStringLiteral("archive_cache")))
            << "an ignored file was reported as an uncovered one: "
            << p.toStdString();
    EXPECT_EQ(gap.bySuffix.value(QStringLiteral("py")), 0);
}

// ANTS-4804 — a lane's SIZE is measured, so the one-huge-file lane that
// file_count cannot see is not handed back as a single reviewable unit.
TEST(IndieReviewComputedPartition, Ants4804LaneLinesAreCountedNotJustFiles) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(QDir(root).filePath(QStringLiteral("src")));

    // One file, many lines — the Rolodex shape: file_count 1, and a whole
    // application behind it.
    QByteArray big;
    for (int i = 0; i < 3000; ++i) big += "int x" + QByteArray::number(i) + ";\n";
    writeFile(root, QStringLiteral("src/huge.py"), big);
    writeFile(root, QStringLiteral("src/small.py"), QByteArray("int y;\n"));

    IndieReviewEngine::Lane huge;
    huge.name        = QStringLiteral("huge");
    huge.sourcePaths = QStringList{QStringLiteral("src/huge.py")};
    IndieReviewEngine::Lane small;
    small.name        = QStringLiteral("small");
    small.sourcePaths = QStringList{QStringLiteral("src/small.py")};

    EXPECT_EQ(IndieReviewEngine::laneFileCount(root, huge), 1)
        << "the premise: by file count this lane looks trivial";
    bool capped = true;
    EXPECT_EQ(IndieReviewEngine::laneLineCount(root, huge, &capped), 3000);
    EXPECT_FALSE(capped) << "a 3k-line file must not hit the scan budget";
    EXPECT_GT(IndieReviewEngine::laneLineCount(root, huge),
              IndieReviewEngine::kMaxReviewableLinesPerLane)
        << "a whole-application lane must trip the line budget";
    EXPECT_LT(IndieReviewEngine::laneLineCount(root, small),
              IndieReviewEngine::kMaxReviewableLinesPerLane)
        << "a real lane must not";
}

// ANTS-4804 — a final line with no trailing newline is still a line, so the
// count does not depend on how the last file happens to end.
TEST(IndieReviewComputedPartition, Ants4804UnterminatedLastLineCounts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    writeFile(root, QStringLiteral("src/a.py"), QByteArray("one\ntwo\nthree"));
    writeFile(root, QStringLiteral("src/b.py"), QByteArray("one\ntwo\nthree\n"));

    IndieReviewEngine::Lane a;
    a.name        = QStringLiteral("a");
    a.sourcePaths = QStringList{QStringLiteral("src/a.py")};
    IndieReviewEngine::Lane b;
    b.name        = QStringLiteral("b");
    b.sourcePaths = QStringList{QStringLiteral("src/b.py")};

    EXPECT_EQ(IndieReviewEngine::laneLineCount(root, a), 3);
    EXPECT_EQ(IndieReviewEngine::laneLineCount(root, b), 3);
}

// INV-9 — a DECLARED partition is measured for coverage too (ANTS-4786).
TEST(IndieReviewComputedPartition, Inv9DeclaredPartitionCoverageIsMeasured) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("src/engine/core.cpp"),
        QStringLiteral("src/engine/core.h"),
        QStringLiteral("src/ui/window.cpp"),
        QStringLiteral("src/tools/run.sh"),
    });

    // One lane naming one directory — the shape a module map produces, and the
    // shape whose coverage nothing could previously ask about.
    IndieReviewEngine::Lane engine;
    engine.name        = QStringLiteral("engine");
    engine.sourcePaths = QStringList{QStringLiteral("src/engine")};

    QStringList sample;
    const auto gap = IndieReviewEngine::unassignedForLanes(
        root, QList<IndieReviewEngine::Lane>{engine}, &sample);

    EXPECT_FALSE(gap.bySuffix.contains(QStringLiteral("h")))
        << "a header inside the named directory IS covered by that lane";
    EXPECT_EQ(gap.bySuffix.value(QStringLiteral("cpp")), 1)
        << "src/ui/window.cpp is in no lane";
    // No suffix filter on this path: a shell script no lane names is a gap,
    // and calling it out of scope is the silence ANTS-4771 was filed about.
    EXPECT_EQ(gap.bySuffix.value(QStringLiteral("sh")), 1);
    EXPECT_EQ(gap.count, 2) << "count is the sum across suffixes";

    EXPECT_TRUE(sample.contains(QStringLiteral("src/tools/run.sh")));
    EXPECT_TRUE(sample.contains(QStringLiteral("src/ui/window.cpp")));
    EXPECT_FALSE(sample.contains(QStringLiteral("src/engine/core.cpp")));
}

// INV-9 — a partition that covers everything reports nothing, so the fields'
// absence is a clean bill of health rather than a question nobody asked.
TEST(IndieReviewComputedPartition, Inv9FullCoverageReportsNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProse(tmp, QStringList{
        QStringLiteral("src/engine/core.cpp"),
        QStringLiteral("src/ui/window.cpp"),
    });

    IndieReviewEngine::Lane all;
    all.name        = QStringLiteral("all");
    all.sourcePaths = QStringList{QStringLiteral("src")};

    const auto gap = IndieReviewEngine::unassignedForLanes(
        root, QList<IndieReviewEngine::Lane>{all});
    EXPECT_EQ(gap.count, 0);
    EXPECT_TRUE(gap.bySuffix.isEmpty());
}

// INV-9 — the sample is sorted BEFORE it is capped, so it does not depend on
// directory-walk order: two runs over one tree must name the same files.
TEST(IndieReviewComputedPartition, Inv9SampleIsDeterministicAndCapped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QStringList files;
    for (int i = 0; i < 25; ++i)
        files << QStringLiteral("src/wide/f%1.cpp")
                     .arg(i, 2, 10, QLatin1Char('0'));
    const QString root = seedProse(tmp, files);

    QStringList sample;
    const auto gap = IndieReviewEngine::unassignedForLanes(
        root, QList<IndieReviewEngine::Lane>{}, &sample);

    EXPECT_EQ(gap.count, 25) << "the count is complete even when the sample is not";
    EXPECT_EQ(sample.size(), 20) << "sample is bounded";
    // Sorted-then-capped means the first names, not whichever 20 the walk hit.
    EXPECT_EQ(sample.first(), QStringLiteral("src/wide/f00.cpp"));
    EXPECT_EQ(sample.last(), QStringLiteral("src/wide/f19.cpp"));

    QStringList again;
    IndieReviewEngine::unassignedForLanes(
        root, QList<IndieReviewEngine::Lane>{}, &again);
    EXPECT_EQ(again, sample) << "two runs over one tree agree";
}

// INV-8 — the handler surfaces the uncovered files for whichever partition the
// reply carries (ANTS-4771; both paths since ANTS-4786).
TEST(IndieReviewComputedPartition, Inv8HandlerReportsUnassignedForTheCarriedPartition) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const std::string body =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdIndieReviewPartition");
    ASSERT_FALSE(body.empty());

    // The reporter must actually be passed to the walk, or it stays empty and
    // every assertion below would pass against a permanently silent envelope.
    EXPECT_NE(body.find("deriveComputedPartition(root, &unassigned)"),
              std::string::npos)
        << "the walk must be given somewhere to record what it skipped";
    EXPECT_NE(body.find("env[\"unassigned_count\"]"), std::string::npos);
    EXPECT_NE(body.find("env[\"unassigned_by_suffix\"]"), std::string::npos)
        << "a bare count cannot tell shell from documentation";

    // ANTS-4786 — the reported set must describe the partition the reply
    // carries. The computed walk answers for `derived`; the declared lanes are
    // measured by unassignedForLanes. Without the second branch the map path
    // reports nothing, which is the defect, and without the first the counts
    // would describe a partition the reply does not carry.
    const auto branch = body.find("if (!derived) {");
    ASSERT_NE(branch, std::string::npos);
    EXPECT_NE(body.find("IndieReviewEngine::unassignedForLanes(root, lanes"),
              std::string::npos)
        << "the declared partition must be measured against the tree";
    // NB: not `emit` — that is a Qt macro and expands to nothing here.
    const auto emitAt = body.find("env[\"unassigned_count\"]");
    ASSERT_NE(emitAt, std::string::npos);
    EXPECT_LT(branch, emitAt) << "measure before reporting";
    EXPECT_NE(body.find("env[\"unassigned_reason\"]"), std::string::npos)
        << "one number, two causes — the caller cannot act without knowing which";
}

// INV-5 — the handler labels the computed partition and keeps the hint.
TEST(IndieReviewComputedPartition, Inv5HandlerLabelsDerivedAndKeepsHint) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const auto fn = rc.find("RemoteControl::cmdIndieReviewPartition");
    ASSERT_NE(fn, std::string::npos);
    // ANTS-4100 — was `rc.substr(fn, 4000)`. Adding the too-coarse signal to
    // this handler pushed `env["derived"]` past byte 4000 and reddened this
    // test, which asserts nothing about that signal: the fixed window measured
    // the function's LENGTH, not its behaviour. slurpFunctionBody brace-matches
    // the real body, which is what srcgrep.h exists for.
    const std::string body =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdIndieReviewPartition");
    ASSERT_FALSE(body.empty());

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
