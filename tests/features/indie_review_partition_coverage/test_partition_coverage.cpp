// ANTS-4794 — ANTS-4793 committed .indie-review/partition.json; nothing kept
// it true. It is a static list of paths, so the day a source file is added it
// belongs to no lane and every later review sweep skips it while reporting a
// clean partition. See spec.md for why no review run can catch that, and why
// the reviewable-file set is taken from the engine rather than restated here.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "indiereviewengine.h"

#ifndef ANTS_PARTITION_JSON_PATH
#error "ANTS_PARTITION_JSON_PATH compile definition required"
#endif
#ifndef ANTS_PROJECT_ROOT_DIR
#error "ANTS_PROJECT_ROOT_DIR compile definition required"
#endif

namespace {

struct Committed {
    QSet<QString> paths;
    QStringList   duplicates;  // named by more than one lane
    int           laneCount = 0;
    QString       error;       // non-empty means nothing else is meaningful
};

// Read the committed override the way IndieReviewEngine reads it: version 1,
// a "lanes" array, each lane's "sourcePaths" project-relative. Deliberately
// NOT a call into the engine's own parser -- that one filters lanes and can
// return a partition smaller than the file, which is the wrong question here.
Committed readCommitted(const QString &path) {
    Committed out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("cannot open %1").arg(path);
        return out;
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        out.error = QStringLiteral("parse error: %1").arg(perr.errorString());
        return out;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        out.error = QStringLiteral("\"version\" must be 1");
        return out;
    }
    const QJsonArray lanes = root.value(QStringLiteral("lanes")).toArray();
    out.laneCount = lanes.size();
    for (const auto &lv : lanes) {
        const QJsonObject lane = lv.toObject();
        const QString name = lane.value(QStringLiteral("name")).toString();
        for (const auto &pv :
                 lane.value(QStringLiteral("sourcePaths")).toArray()) {
            const QString rel = pv.toString();
            if (rel.isEmpty()) continue;
            if (out.paths.contains(rel))
                out.duplicates << QStringLiteral("%1 (again in lane \"%2\")")
                                      .arg(rel, name);
            out.paths.insert(rel);
        }
    }
    return out;
}

QString projectRoot() {
    // canonicalFilePath, because deriveComputedPartition compares the walked
    // absolute paths against this prefix to build its relative ones -- an
    // uncanonical root yields zero relative paths and a vacuous pass.
    return QFileInfo(QString::fromUtf8(ANTS_PROJECT_ROOT_DIR))
        .canonicalFilePath();
}

// Up to `cap` entries, sorted, for a failure message that stays readable when
// somebody adds a directory rather than a file.
QString sample(QStringList v, int cap = 12) {
    v.sort();
    const int extra = v.size() - cap;
    if (extra > 0) {
        v = v.mid(0, cap);
        v << QStringLiteral("... and %1 more").arg(extra);
    }
    return v.join(QStringLiteral("\n  "));
}

}  // namespace

// INV-1 — every file the engine would place in a lane is in some lane of the
// committed partition. The one that fires when a source file is added.
TEST(IndieReviewPartitionCoverage, EveryReviewableSourceFileHasALane) {
    const QString root = projectRoot();
    ASSERT_FALSE(root.isEmpty()) << ANTS_PROJECT_ROOT_DIR << " does not resolve";

    const Committed committed =
        readCommitted(QString::fromUtf8(ANTS_PARTITION_JSON_PATH));
    ASSERT_TRUE(committed.error.isEmpty())
        << ".indie-review/partition.json: "
        << committed.error.toStdString();
    ASSERT_GT(committed.laneCount, 0) << "the committed partition declares no lanes";

    QSet<QString> derived;
    for (const auto &lane : IndieReviewEngine::deriveComputedPartition(root))
        for (const QString &rel : lane.sourcePaths) derived.insert(rel);

    // Guard the guard. An empty walk would make every assertion below pass
    // while checking nothing -- the vacuous-pass failure this whole test
    // exists to prevent one level up.
    ASSERT_FALSE(derived.isEmpty())
        << "deriveComputedPartition returned no files for " << root.toStdString()
        << " -- the walk, not the partition, is what is broken here";

    QStringList missing;
    for (const QString &rel : derived)
        if (!committed.paths.contains(rel)) missing << rel;

    EXPECT_TRUE(missing.isEmpty())
        << missing.size() << " source file(s) belong to no review lane:\n  "
        << sample(missing).toStdString()
        << "\n\nAdd each to a lane's \"sourcePaths\" in "
           ".indie-review/partition.json. A file in no lane is dispatched to "
           "nobody: every review sweep silently skips it and still reports a "
           "clean partition (ANTS-4793, and the same shape as ANTS-4785/4786).";
}

// INV-2 — no lane names a path that is gone. The one that fires on a rename
// or a delete, which is equally invisible to a review run.
TEST(IndieReviewPartitionCoverage, NoLaneNamesAPathThatIsGone) {
    const QString root = projectRoot();
    ASSERT_FALSE(root.isEmpty());

    const Committed committed =
        readCommitted(QString::fromUtf8(ANTS_PARTITION_JSON_PATH));
    ASSERT_TRUE(committed.error.isEmpty()) << committed.error.toStdString();
    ASSERT_FALSE(committed.paths.isEmpty()) << "the partition names no paths";

    QStringList gone;
    for (const QString &rel : committed.paths)
        if (!QFileInfo(QDir(root).filePath(rel)).isFile()) gone << rel;

    EXPECT_TRUE(gone.isEmpty())
        << gone.size() << " lane path(s) no longer exist:\n  "
        << sample(gone).toStdString()
        << "\n\nA renamed or deleted file leaves its lane pointing at nothing "
           "and its replacement in no lane at all. Update "
           ".indie-review/partition.json in the commit that moves the file.";
}

// INV-3 — a partition, not a covering. A duplicate is the signal that the
// author lost track of which lane owns what.
TEST(IndieReviewPartitionCoverage, EveryPathBelongsToExactlyOneLane) {
    const Committed committed =
        readCommitted(QString::fromUtf8(ANTS_PARTITION_JSON_PATH));
    ASSERT_TRUE(committed.error.isEmpty()) << committed.error.toStdString();

    EXPECT_TRUE(committed.duplicates.isEmpty())
        << committed.duplicates.size() << " path(s) named by more than one "
           "lane:\n  "
        << sample(committed.duplicates).toStdString();
}
