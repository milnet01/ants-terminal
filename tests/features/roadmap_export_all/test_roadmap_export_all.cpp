// Feature-conformance test for ANTS-3794 INV-1..INV-6 — exporting every
// project, and the --export-roadmaps command around it.
// Contract: tests/features/roadmap_export_all/spec.md

#include <gtest/gtest.h>

#include "roadmapexport.h"
#include "roadmapstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTextStream>

#include <memory>

#ifndef ANTS_SRC_DIR
#  error "ANTS_SRC_DIR compile definition required"
#endif

namespace {

QByteArray readAll(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// A store at `storePath` holding one project per slug. Returned open, so a
// test can call exportAllProjects() on it directly.
std::unique_ptr<RoadmapStore> storeWith(const QString &storePath,
                                        const QStringList &slugs) {
    auto store = std::make_unique<RoadmapStore>(
        storePath, RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << err.toStdString();
        return store;
    }
    const QString base = QFileInfo(storePath).absolutePath();
    for (const QString &slug : slugs) {
        const QString root = base + QStringLiteral("/root-") + slug;
        QDir().mkpath(root);
        if (!store->registerProject(root, slug, slug, &err).has_value())
            ADD_FAILURE() << err.toStdString();
    }
    return store;
}

QStringList sorted(QStringList l) {
    l.sort();
    return l;
}

}  // namespace

// INV-1 — the flag is handled before QApplication exists, so a systemd user
// service with no display can run it.
TEST(RoadmapExportAll, Inv1FlagHandledBeforeQApplication) {
    const QString src = QString::fromUtf8(
        readAll(QStringLiteral(ANTS_SRC_DIR) + QStringLiteral("/main.cpp")));
    ASSERT_FALSE(src.isEmpty());
    const qsizetype flag = src.indexOf(QStringLiteral("--export-roadmaps"));
    const qsizetype app = src.indexOf(QStringLiteral("QApplication app("));
    ASSERT_GE(app, 0);
    ASSERT_GE(flag, 0) << "main.cpp never mentions --export-roadmaps";
    EXPECT_LT(flag, app) << "--export-roadmaps must be handled before QApplication";
}

// INV-2 — one file per project, byte-identical to exportProject().
TEST(RoadmapExportAll, Inv2OneFilePerProjectMatchingExportProject) {
    QTemporaryDir tmp;
    auto store = storeWith(tmp.path() + QStringLiteral("/roadmap.sqlite"),
                           {QStringLiteral("beta"), QStringLiteral("alpha")});
    const QString dir = tmp.path() + QStringLiteral("/out");

    const auto r = RoadmapExport::exportAllProjects(*store, dir);
    EXPECT_TRUE(r.error.isEmpty()) << r.error.toStdString();
    EXPECT_TRUE(r.failed.isEmpty());
    EXPECT_EQ(r.written, (QStringList{QStringLiteral("alpha"), QStringLiteral("beta")}));

    for (const QString &slug : {QStringLiteral("alpha"), QStringLiteral("beta")}) {
        const QString ref = tmp.path() + QStringLiteral("/ref-") + slug + QStringLiteral(".jsonl");
        QString err;
        ASSERT_TRUE(RoadmapExport::exportProject(*store, slug, ref, &err)) << err.toStdString();
        const QByteArray got = readAll(dir + QStringLiteral("/") + slug + QStringLiteral(".jsonl"));
        EXPECT_FALSE(got.isEmpty()) << slug.toStdString();
        EXPECT_EQ(got, readAll(ref)) << slug.toStdString();
    }
}

// INV-3 — alpha's destination is a directory, so only alpha's write fails;
// beta still exports and the command returns 1.
TEST(RoadmapExportAll, Inv3OneFailureDoesNotStopTheOthers) {
    QTemporaryDir tmp;
    const QString storePath = tmp.path() + QStringLiteral("/roadmap.sqlite");
    const QString dir = tmp.path() + QStringLiteral("/out");
    {
        auto store = storeWith(storePath, {QStringLiteral("alpha"), QStringLiteral("beta")});
        ASSERT_TRUE(QDir().mkpath(dir + QStringLiteral("/alpha.jsonl")));

        const auto r = RoadmapExport::exportAllProjects(*store, dir);
        ASSERT_EQ(r.failed.size(), 1);
        EXPECT_TRUE(r.failed.first().startsWith(QStringLiteral("alpha:")))
            << r.failed.first().toStdString();
        EXPECT_EQ(r.written, QStringList{QStringLiteral("beta")});
        EXPECT_FALSE(readAll(dir + QStringLiteral("/beta.jsonl")).isEmpty());
    }
    QString out;
    QTextStream ts(&out);
    EXPECT_EQ(RoadmapExport::runExportCommand(storePath, dir, ts), 1);
}

// INV-4 — no store file: return 2 and create nothing.
TEST(RoadmapExportAll, Inv4NoStoreReturnsTwoAndCreatesNone) {
    QTemporaryDir tmp;
    const QString storePath = tmp.path() + QStringLiteral("/roadmap.sqlite");
    QString out;
    QTextStream ts(&out);
    EXPECT_EQ(RoadmapExport::runExportCommand(storePath, tmp.path() + QStringLiteral("/out"), ts), 2);
    EXPECT_FALSE(QFileInfo::exists(storePath)) << "the command created a store";
}

// INV-5 — an empty project list is a fault: nothing written or deleted.
TEST(RoadmapExportAll, Inv5EmptyStoreTouchesNothing) {
    QTemporaryDir tmp;
    auto store = storeWith(tmp.path() + QStringLiteral("/roadmap.sqlite"), {});
    const QString dir = tmp.path() + QStringLiteral("/out");
    ASSERT_TRUE(QDir().mkpath(dir));
    QFile a(dir + QStringLiteral("/a.jsonl"));
    ASSERT_TRUE(a.open(QIODevice::WriteOnly));
    a.write("keep\n");
    a.close();

    const auto r = RoadmapExport::exportAllProjects(*store, dir);
    EXPECT_FALSE(r.error.isEmpty()) << "an empty project list must be reported";
    EXPECT_TRUE(r.written.isEmpty());
    EXPECT_TRUE(r.removed.isEmpty());
    EXPECT_EQ(readAll(dir + QStringLiteral("/a.jsonl")), QByteArray("keep\n"));
}

// INV-6 — orphans go only after a clean run; other files never.
TEST(RoadmapExportAll, Inv6OrphansRemovedOnlyAfterACleanRun) {
    QTemporaryDir tmp;
    auto store = storeWith(tmp.path() + QStringLiteral("/roadmap.sqlite"),
                           {QStringLiteral("alpha"), QStringLiteral("beta")});
    const QString dir = tmp.path() + QStringLiteral("/out");
    ASSERT_TRUE(QDir().mkpath(dir + QStringLiteral("/sub")));
    for (const QString &name : {QStringLiteral("orphan.jsonl"), QStringLiteral("notes.txt"),
                                QStringLiteral("sub/x.jsonl")}) {
        QFile f(dir + QStringLiteral("/") + name);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("x\n");
    }

    // A failed run (alpha's destination is a directory) deletes nothing.
    ASSERT_TRUE(QDir().mkpath(dir + QStringLiteral("/alpha.jsonl")));
    auto r = RoadmapExport::exportAllProjects(*store, dir);
    EXPECT_EQ(r.failed.size(), 1);
    EXPECT_TRUE(r.removed.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/orphan.jsonl")));

    // A clean run removes the orphan and only the orphan.
    ASSERT_TRUE(QDir(dir + QStringLiteral("/alpha.jsonl")).removeRecursively());
    r = RoadmapExport::exportAllProjects(*store, dir);
    EXPECT_TRUE(r.failed.isEmpty());
    EXPECT_EQ(r.removed, QStringList{QStringLiteral("orphan.jsonl")});
    EXPECT_FALSE(QFileInfo::exists(dir + QStringLiteral("/orphan.jsonl")));
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/notes.txt")));
    EXPECT_TRUE(QFileInfo::exists(dir + QStringLiteral("/sub/x.jsonl")));
    EXPECT_EQ(sorted(QDir(dir).entryList({QStringLiteral("*.jsonl")}, QDir::Files)),
              (QStringList{QStringLiteral("alpha.jsonl"), QStringLiteral("beta.jsonl")}));
}
