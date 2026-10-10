// Feature-conformance test for ANTS-5244 INV-1..INV-4 — restoring one project
// from its export with --import-roadmap.
// Contract: tests/features/roadmap_import_command/spec.md

#include <gtest/gtest.h>

#include "roadmapexport.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#ifndef ANTS_SRC_DIR
#  error "ANTS_SRC_DIR compile definition required"
#endif
#ifndef ANTS_EXPORT_GOLDEN_DIR
#  error "ANTS_EXPORT_GOLDEN_DIR compile definition required"
#endif

namespace {

QByteArray readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A committed export holding every record type, including a cross-project
// link, so a restore that drops any of them changes the re-export.
QString goldenAlpha() {
    return QStringLiteral(ANTS_EXPORT_GOLDEN_DIR) + QStringLiteral("/alpha.jsonl");
}

int runImport(const QString &store, const QString &file, const QString &root,
              QString *printed = nullptr) {
    QString text;
    QTextStream ts(&text);
    const int rc = RoadmapExport::runImportCommand(store, file, root, ts);
    ts.flush();
    if (printed)
        *printed = text;  // a copy: `ts` still writes into `text`
    return rc;
}

}  // namespace

// INV-1 — handled before QApplication, like --export-roadmaps.
TEST(RoadmapImportCommand, Inv1FlagHandledBeforeQApplication) {
    const QString src = QString::fromUtf8(
        readAll(QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/main.cpp")));
    const qsizetype flag = src.indexOf(QStringLiteral("\"--import-roadmap\""));
    const qsizetype app  = src.indexOf(QStringLiteral("QApplication app("));
    ASSERT_GE(flag, 0) << "main.cpp never handles --import-roadmap";
    ASSERT_GE(app, 0);
    EXPECT_LT(flag, app) << "--import-roadmap must be handled before QApplication";
}

// INV-2 — into a store that does not exist yet: exit 0, findable by root, and
// the re-export is byte-identical to the file restored.
TEST(RoadmapImportCommand, Inv2RestoreIsFaithfulAndFindable) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString storePath = tmp.path() + QStringLiteral("/store/roadmap.sqlite");
    const QString root = tmp.path() + QStringLiteral("/alpha-root");
    ASSERT_TRUE(QDir().mkpath(root));
    ASSERT_FALSE(QFileInfo::exists(storePath));

    QString printed;
    ASSERT_EQ(runImport(storePath, goldenAlpha(), root, &printed), 0)
        << printed.toStdString();
    EXPECT_TRUE(printed.contains(QStringLiteral("roadmap_log op:\"render\"")))
        << printed.toStdString();

    RoadmapStore store(storePath, RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    const auto byRoot = store.projectIdForRoot(root, &err);
    const auto bySlug = store.projectIdForSlug(QStringLiteral("alpha"), &err);
    ASSERT_TRUE(byRoot.has_value()) << "restored project is not findable by its root";
    ASSERT_TRUE(bySlug.has_value());
    EXPECT_EQ(*byRoot, *bySlug);

    const QString again = tmp.path() + QStringLiteral("/alpha-again.jsonl");
    ASSERT_TRUE(RoadmapExport::exportProject(store, QStringLiteral("alpha"), again, &err))
        << err.toStdString();
    EXPECT_EQ(readAll(again), readAll(goldenAlpha()));
}

// INV-3 — the export's project already present, or the root already held by
// another project: exit 1, the store unchanged. The deregister route is named
// only where one row holds both the root and the slug (ANTS-5366 § 2.6):
// deregistering frees neither, so elsewhere it would lead nowhere.
TEST(RoadmapImportCommand, Inv3RestoreNeverOverwrites) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString storePath = tmp.path() + QStringLiteral("/roadmap.sqlite");
    const QString rootA = tmp.path() + QStringLiteral("/a");
    const QString rootB = tmp.path() + QStringLiteral("/b");
    ASSERT_TRUE(QDir().mkpath(rootA));
    ASSERT_TRUE(QDir().mkpath(rootB));
    ASSERT_EQ(runImport(storePath, goldenAlpha(), rootA), 0);

    // Same project again, at a fresh root.
    QString printed;
    EXPECT_EQ(runImport(storePath, goldenAlpha(), rootB, &printed), 1);
    EXPECT_FALSE(printed.contains(QStringLiteral("op:\"deregister\""))) << printed.toStdString();
    EXPECT_TRUE(printed.contains(QStringLiteral("restore into another store")))
        << printed.toStdString();

    // Same project again, at its own root: one row holds both, so deregistering
    // then restoring is a real route and is named.
    EXPECT_EQ(runImport(storePath, goldenAlpha(), rootA, &printed), 1);
    EXPECT_TRUE(printed.contains(QStringLiteral("op:\"deregister\""))) << printed.toStdString();

    // A different project at a root the store already holds.
    {
        RoadmapStore store(storePath, RoadmapStore::kDefaultHistoryCapBytes,
                           RoadmapStore::Access::Bulk);
        QString err;
        ASSERT_TRUE(store.open(&err)) << err.toStdString();
        ASSERT_TRUE(store.registerProject(rootB, QStringLiteral("Other"),
                                          QStringLiteral("other"), &err).has_value())
            << err.toStdString();
    }
    const QString renamed = tmp.path() + QStringLiteral("/renamed.jsonl");
    {
        QByteArray body = readAll(goldenAlpha());
        body.replace("\"name\":\"Alpha\",\"project\":\"alpha\"",
                     "\"name\":\"Delta\",\"project\":\"delta\"");
        QFile f(renamed);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    EXPECT_EQ(runImport(storePath, renamed, rootB, &printed), 1);
    EXPECT_FALSE(printed.contains(QStringLiteral("op:\"deregister\""))) << printed.toStdString();

    RoadmapStore store(storePath, RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    EXPECT_EQ(store.projectIdForRoot(rootA, &err), store.projectIdForSlug(QStringLiteral("alpha"), &err));
    EXPECT_EQ(store.projectIdForRoot(rootB, &err), store.projectIdForSlug(QStringLiteral("other"), &err));
    EXPECT_FALSE(store.projectIdForSlug(QStringLiteral("delta"), &err).has_value())
        << "a refused restore left its project row behind";
}

// INV-4 — an unreadable file or a root that is not a directory: exit 2, and
// no store is created.
TEST(RoadmapImportCommand, Inv4BadArgumentsCreateNoStore) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString storePath = tmp.path() + QStringLiteral("/roadmap.sqlite");
    const QString root = tmp.path() + QStringLiteral("/root");
    ASSERT_TRUE(QDir().mkpath(root));

    EXPECT_EQ(runImport(storePath, tmp.path() + QStringLiteral("/missing.jsonl"), root), 2);
    EXPECT_EQ(runImport(storePath, goldenAlpha(), tmp.path() + QStringLiteral("/no-such-dir")), 2);
    EXPECT_EQ(runImport(storePath, goldenAlpha(), goldenAlpha()), 2) << "a file is not a root";
    EXPECT_FALSE(QFileInfo::exists(storePath));
}
