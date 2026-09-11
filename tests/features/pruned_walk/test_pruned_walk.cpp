// ANTS-5058 / ANTS-5062 — PrunedWalk::walkFiles must never descend into a
// directory it is told to prune. See
// tests/features/pruned_walk/spec.md.
//
// Why this exists: both IndieReviewEngine's corroboration walk and
// TestAuditEngine's partition walk used to list every file under `build/`
// (and every other build tree) with QDirIterator before deciding it was
// noise. This test drives the shared primitive both are meant to route
// through, directly, so a future caller cannot silently go back to
// list-then-filter.

#include <gtest/gtest.h>

#include "prunedwalk.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

void touch(const QString &root, const QString &rel) {
    const QFileInfo fi(QDir(root).filePath(rel));
    QDir().mkpath(fi.absolutePath());
    QFile f(fi.absoluteFilePath());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("// fixture\n");
}

bool isBuildDir(const QString &name) { return name == QLatin1String("build"); }

}  // namespace

// INV-1 — a pruned directory is never descended: its files are never
// visited, and the walk never touches nearly as many entries as sit under
// it, since it must not have listed them at all.
TEST(PrunedWalk, Inv1PrunedDirNeverDescended) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    touch(root, QStringLiteral("src/a.cpp"));
    touch(root, QStringLiteral("build/b1.cpp"));
    touch(root, QStringLiteral("build/b2.cpp"));
    touch(root, QStringLiteral("build/b3.cpp"));
    touch(root, QStringLiteral("build/sub/s1.cpp"));
    touch(root, QStringLiteral("build/sub/s2.cpp"));
    touch(root, QStringLiteral("build/sub/s3.cpp"));
    touch(root, QStringLiteral("build/sub/s4.cpp"));
    constexpr int kFilesUnderBuild = 7;

    QStringList visited;
    const auto stats = PrunedWalk::walkFiles(
        root, isBuildDir,
        [&](const QString &absPath) { visited << absPath; return true; });

    // GUARD — even today's stub drops a pruned file before calling the file
    // callback (it filters after listing, but it does filter); this already
    // passes and must keep passing.
    for (const QString &p : visited)
        EXPECT_FALSE(p.contains(QStringLiteral("/build/")))
            << "file callback ran for a pruned path: " << p.toStdString();

    // The walk itself must never touch what it prunes. Today's stub lists
    // every file under `build/` with QDirIterator before dropping it, so
    // entriesVisited counts all 7 of them plus src/a.cpp (8), not below 7.
    EXPECT_LT(stats.entriesVisited, kFilesUnderBuild)
        << "entriesVisited=" << stats.entriesVisited << " must stay below "
        << kFilesUnderBuild << " (the files under the pruned directory) — "
        << "the walk touched what it was told to prune";
}

// INV-2 — the cap counts entries the walk visits, not files it accepts. A
// pruned directory holding more files than the cap must not spend it: the
// one accepted file outside the pruned directory is still reached, and the
// walk does not report itself as capped.
//
// Deliberately asserted on `capped` rather than on directory-iteration
// order: a correct walk never lists the pruned directory's files at all, so
// the cap is never spent on them regardless of which directory the
// filesystem happens to enumerate first. Today's stub counts every file
// BEFORE deciding to prune it, so with 5 pruned files + 1 accepted file
// against a cap of 3, the cap trips before the walk finishes — in every
// enumeration order, since 6 total files exceeds the cap of 3 however they
// are ordered.
TEST(PrunedWalk, Inv2CapCountsVisitedNotAccepted) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    touch(root, QStringLiteral("build/b1.cpp"));
    touch(root, QStringLiteral("build/b2.cpp"));
    touch(root, QStringLiteral("build/b3.cpp"));
    touch(root, QStringLiteral("build/b4.cpp"));
    touch(root, QStringLiteral("build/b5.cpp"));
    touch(root, QStringLiteral("keep/a.cpp"));
    constexpr int kMaxEntries = 3;   // below the 5 pruned files, above the 1 accepted

    QStringList visited;
    const auto stats = PrunedWalk::walkFiles(
        root, isBuildDir,
        [&](const QString &absPath) { visited << absPath; return true; },
        kMaxEntries);

    EXPECT_FALSE(stats.capped)
        << "entriesVisited=" << stats.entriesVisited << " capped=" << stats.capped
        << " — a pruned directory's files must not spend the entry cap";
    EXPECT_TRUE(visited.contains(QDir(root).filePath(QStringLiteral("keep/a.cpp"))))
        << "the one accepted file outside the pruned directory must still "
           "be reached; visited=" << visited.join(QStringLiteral(", ")).toStdString();
}
