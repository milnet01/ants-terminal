// Feature-conformance test for tests/features/indie_review_corroborate_dir/spec.md.
//
// ANTS-1282: corroboratedFindingsFromDir reads per-lane reports from
// a directory and corroborates server-side, saving parent context.

#include "indiereviewengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

bool writeMd(const QString &dir, const QString &name,
             const QByteArray &body) {
    QFile f(dir + QChar('/') + name);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(body);
    f.close();
    return true;
}

// Build a 3-lane minimal fake project with one shared (file, line)
// citation from two lanes — so corroborate(min_lanes=2) returns 1
// finding.
QString makeProjectWithReports(QTemporaryDir &tmp,
                                const QString &reportsSubdir) {
    const QString root = tmp.path();
    QDir(root).mkpath(QStringLiteral("src"));
    QDir(root).mkpath(reportsSubdir);
    // Materialise the cited source file so canonicalisation succeeds
    // (extractFileLineCitations rejects paths that don't resolve
    // under projectPath).
    QFile srcFile(root + QStringLiteral("/src/vtparser.cpp"));
    EXPECT_TRUE(srcFile.open(QIODevice::WriteOnly));
    srcFile.write(QByteArray(64, 'X'));
    srcFile.close();

    const QString rdir = root + QChar('/') + reportsSubdir;
    EXPECT_TRUE(writeMd(rdir, QStringLiteral("vtparser.md"),
        QByteArray("INV-A violated at src/vtparser.cpp:42 - bug here.\n")));
    EXPECT_TRUE(writeMd(rdir, QStringLiteral("terminalgrid.md"),
        QByteArray("Independent reviewer also flags src/vtparser.cpp:42.\n")));
    EXPECT_TRUE(writeMd(rdir, QStringLiteral("audit.md"),
        QByteArray("No comment.\n")));
    return root;
}

}  // namespace

TEST(IndieReviewCorroborateDir, OutputShapeParityWithInlineMap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = makeProjectWithReports(
        tmp, QStringLiteral(".indie-review-test"));

    // Disk path.
    int read = 0;
    const auto diskFound = IndieReviewEngine::corroboratedFindingsFromDir(
        root, QStringLiteral(".indie-review-test"), 2, &read);
    EXPECT_EQ(read, 3);
    ASSERT_EQ(diskFound.size(), 1);
    EXPECT_EQ(diskFound.first().file,
              QStringLiteral("src/vtparser.cpp"));
    EXPECT_EQ(diskFound.first().line, 42);
    EXPECT_EQ(diskFound.first().citingLanes.size(), 2);

    // Inline path — same reports built as an in-memory map.
    QHash<QString, QString> reports;
    QFile a(root + QStringLiteral("/.indie-review-test/vtparser.md"));
    QFile b(root + QStringLiteral("/.indie-review-test/terminalgrid.md"));
    QFile c(root + QStringLiteral("/.indie-review-test/audit.md"));
    ASSERT_TRUE(a.open(QIODevice::ReadOnly));
    ASSERT_TRUE(b.open(QIODevice::ReadOnly));
    ASSERT_TRUE(c.open(QIODevice::ReadOnly));
    reports.insert(QStringLiteral("vtparser"),     QString::fromUtf8(a.readAll()));
    reports.insert(QStringLiteral("terminalgrid"), QString::fromUtf8(b.readAll()));
    reports.insert(QStringLiteral("audit"),        QString::fromUtf8(c.readAll()));
    const auto inlineFound =
        IndieReviewEngine::corroboratedFindings(root, reports, 2);

    // INV-2: same input → same output (modulo lane order, which we
    // sort to compare).
    ASSERT_EQ(inlineFound.size(), diskFound.size());
    EXPECT_EQ(inlineFound.first().file, diskFound.first().file);
    EXPECT_EQ(inlineFound.first().line, diskFound.first().line);
    QStringList diskLanes = diskFound.first().citingLanes;
    QStringList inlineLanes = inlineFound.first().citingLanes;
    diskLanes.sort();
    inlineLanes.sort();
    EXPECT_EQ(diskLanes, inlineLanes);
}

TEST(IndieReviewCorroborateDir, AbsolutePathRejected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    int read = 0;
    const auto found = IndieReviewEngine::corroboratedFindingsFromDir(
        tmp.path(), QStringLiteral("/etc"), 2, &read);
    EXPECT_TRUE(found.isEmpty());
    EXPECT_EQ(read, 0);
}

TEST(IndieReviewCorroborateDir, TraversalEscapeRejected) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    int read = 0;
    const auto found = IndieReviewEngine::corroboratedFindingsFromDir(
        tmp.path(), QStringLiteral("../../../etc"), 2, &read);
    EXPECT_TRUE(found.isEmpty());
    EXPECT_EQ(read, 0);
}

TEST(IndieReviewCorroborateDir, NonMarkdownIgnored) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    QDir(root).mkpath(QStringLiteral("src"));
    QDir(root).mkpath(QStringLiteral("r"));
    // Source file so citation resolves under root.
    QFile sf(root + QStringLiteral("/src/foo.cpp"));
    ASSERT_TRUE(sf.open(QIODevice::WriteOnly));
    sf.write("z");
    sf.close();
    // Reports dir: one .md + one .json + one .txt + one subdir.
    writeMd(root + QStringLiteral("/r"), QStringLiteral("vtparser.md"),
        QByteArray("issue at src/foo.cpp:7\n"));
    QFile js(root + QStringLiteral("/r/summary.json"));
    ASSERT_TRUE(js.open(QIODevice::WriteOnly));
    js.write("{}\n");
    js.close();
    QFile txt(root + QStringLiteral("/r/notes.txt"));
    ASSERT_TRUE(txt.open(QIODevice::WriteOnly));
    txt.write("ignored\n");
    txt.close();
    QDir(root).mkpath(QStringLiteral("r/subdir"));
    writeMd(root + QStringLiteral("/r/subdir"),
            QStringLiteral("nested.md"),
            QByteArray("also at src/foo.cpp:7\n"));

    int read = 0;
    const auto found = IndieReviewEngine::corroboratedFindingsFromDir(
        root, QStringLiteral("r"), 1, &read);

    // Only vtparser.md should have been read — non-md ignored, subdir
    // not recursed.
    EXPECT_EQ(read, 1);
    // One report, one citation → one finding (min_lanes=1).
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.first().citingLanes.size(), 1);
}

TEST(IndieReviewCorroborateDir, MissingDirReturnsEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    int read = 0;
    const auto found = IndieReviewEngine::corroboratedFindingsFromDir(
        tmp.path(), QStringLiteral("does-not-exist"), 2, &read);
    EXPECT_TRUE(found.isEmpty());
    EXPECT_EQ(read, 0);
}

TEST(IndieReviewCorroborateDir, TruncationAt64KiB) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    QDir(root).mkpath(QStringLiteral("src"));
    QDir(root).mkpath(QStringLiteral("r"));
    QFile sf(root + QStringLiteral("/src/early.cpp"));
    ASSERT_TRUE(sf.open(QIODevice::WriteOnly));
    sf.write("z");
    sf.close();
    QFile sf2(root + QStringLiteral("/src/late.cpp"));
    ASSERT_TRUE(sf2.open(QIODevice::WriteOnly));
    sf2.write("z");
    sf2.close();

    // Build a 100 KiB report: citation in first 1 KiB to src/early.cpp,
    // citation past 64 KiB boundary to src/late.cpp.
    QByteArray body;
    body.reserve(100 * 1024);
    body.append("Finding at src/early.cpp:5\n");
    body.append(QByteArray(70 * 1024, ' '));  // pushes past 64 KiB
    body.append("Finding at src/late.cpp:9 — should be invisible\n");
    body.append(QByteArray(20 * 1024, 'x'));
    writeMd(root + QStringLiteral("/r"), QStringLiteral("big.md"), body);

    int read = 0;
    const auto found = IndieReviewEngine::corroboratedFindingsFromDir(
        root, QStringLiteral("r"), 1, &read);
    EXPECT_EQ(read, 1);

    // Both files cited, but only `early` should appear — `late` is
    // past the 64 KiB scan window so extractFileLineCitations skips
    // it.
    bool earlySeen = false;
    bool lateSeen  = false;
    for (const auto &f : found) {
        if (f.file == QStringLiteral("src/early.cpp")) earlySeen = true;
        if (f.file == QStringLiteral("src/late.cpp"))  lateSeen  = true;
    }
    EXPECT_TRUE(earlySeen);
    EXPECT_FALSE(lateSeen)
        << "Citation past 64 KiB was returned; truncation regressed.";
}

// INV-9 (ANTS-3713) — reports may live OUTSIDE the project (the session
// scratchpad), reached via the canonical-dir entry point. INV-3 is unchanged
// on the project-relative entry point, which still refuses the same absolute
// path — asserted in the same test so a future loosening of INV-3 cannot pass
// unnoticed.
TEST(IndieReviewCorroborateDir, Inv9ExternalReportsDirViaCanonicalEntry) {
    QTemporaryDir proj;
    ASSERT_TRUE(proj.isValid());
    QTemporaryDir scratch;   // stands in for /tmp/claude-*/…/scratchpad
    ASSERT_TRUE(scratch.isValid());

    const QString root = proj.path();
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("src")));
    QFile srcFile(root + QStringLiteral("/src/vtparser.cpp"));
    ASSERT_TRUE(srcFile.open(QIODevice::WriteOnly));
    srcFile.write(QByteArray(64, 'X'));
    srcFile.close();

    ASSERT_TRUE(writeMd(scratch.path(), QStringLiteral("vtparser.md"),
        QByteArray("INV-A violated at src/vtparser.cpp:42 - bug here.\n")));
    ASSERT_TRUE(writeMd(scratch.path(), QStringLiteral("terminalgrid.md"),
        QByteArray("Independent reviewer also flags src/vtparser.cpp:42.\n")));

    // INV-3 still holds: the project-relative entry point rejects an
    // absolute path outright, and reports zero files read.
    int readRel = 7;
    const auto viaRelative = IndieReviewEngine::corroboratedFindingsFromDir(
        root, scratch.path(), 2, &readRel);
    EXPECT_TRUE(viaRelative.isEmpty())
        << "INV-3 regressed: corroboratedFindingsFromDir accepted an "
           "absolute reports_dir.";
    EXPECT_EQ(readRel, 0);

    // The canonical entry point reads the same directory — this is the path
    // allow_outside_project takes once PathValidation has anchored it.
    int read = 0;
    const auto found =
        IndieReviewEngine::corroboratedFindingsFromCanonicalDir(
            root, QFileInfo(scratch.path()).canonicalFilePath(), 2, &read);
    EXPECT_EQ(read, 2);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.first().file, QStringLiteral("src/vtparser.cpp"));
    EXPECT_EQ(found.first().line, 42);
}
