// ANTS-5229 — a fold-in on a store-served project writes through the store.
// Contract: tests/features/roadmap_fold_in_store/spec.md.

#include "../../_support/xdg_guard.h"

#include "roadmapfoldin.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmaprender.h"
#include "roadmapstore.h"
#include "roadmapwrite.h"

#include <gtest/gtest.h>

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace {

const char *kRoadmap =
    "# Demo Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0001] **A seed item.**\n"
    "  **Layman:** A seed.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n"
    "\n"
    "## Later\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0002] **A later item.**\n"
    "  **Layman:** Later.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";

const char *kHeading = "## To Do";

const char *kBlock =
    "### Audit fold-in\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0010] **A finding.**\n"
    "  At `src/a.cpp:3` (rule `r`).\n"
    "  Kind: audit-fix.\n"
    "  Source: audit-2026-10-31.\n"
    "  Lanes: a.\n";

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(body) == body.size();
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. SetUp redirects that first.
std::unique_ptr<RoadmapStore> openStore(RoadmapStore::Access access) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes, access);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

class RoadmapFoldInStore : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(tmp.isValid());
        guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
        const QString raw = QDir(tmp.path()).filePath(QStringLiteral("proj"));
        ASSERT_TRUE(writeFile(raw + QStringLiteral("/ROADMAP.md"), kRoadmap));
        root = QFileInfo(raw).canonicalFilePath();
        roadmap = root + QStringLiteral("/ROADMAP.md");
    }

    void migrate() {
        auto store = openStore(RoadmapStore::Access::Bulk);
        ASSERT_TRUE(store);
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        ASSERT_TRUE(disc) << err.toStdString();
        RoadmapMigrateLoad::Options opts;
        opts.changedAt = QStringLiteral("2026-08-19T10:00:00Z");
        opts.projectRoot = root;
        const auto out = RoadmapMigrateLoad::load(
            *store,
            RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo")),
            opts);
        ASSERT_TRUE(out.ok) << out.error.toStdString();
        // The file becomes the render, as it is on any served project.
        ASSERT_EQ(nextWrite(), RoadmapWrite::Result::Ok);
    }

    bool foldIn(const QString &heading, const QString &block) {
        return RoadmapFoldIn::insertBlock(root, heading, block);
    }

    // What the next roadmap_log write does, with nothing of its own to write.
    RoadmapWrite::Result nextWrite() {
        auto store = openStore(RoadmapStore::Access::Interactive);
        if (!store) return RoadmapWrite::Result::StoreFailed;
        QString err;
        const auto row = store->readProjectByRoot(root, &err);
        if (!row) return RoadmapWrite::Result::StoreFailed;
        RoadmapRender::Outcome outcome;
        return RoadmapWrite::commitAndRender(
            *store, row->projectId, root, roadmap, /*dryRun=*/false,
            [](QString *) { return true; }, &outcome, &err);
    }

    std::optional<RoadmapStore::ItemWrite> item(const QString &id) {
        auto store = openStore(RoadmapStore::Access::Interactive);
        if (!store) return std::nullopt;
        const auto row = store->readProjectByRoot(root);
        if (!row) return std::nullopt;
        const auto pk = store->findItem(row->projectId, id);
        return pk ? store->readItem(*pk) : std::nullopt;
    }

    QTemporaryDir tmp;
    ants_test::XdgGuard guard;
    QString root, roadmap;
};

}  // namespace

// INV-1
TEST_F(RoadmapFoldInStore, Inv1NextWriteIsNotRefused) {
    migrate();
    ASSERT_TRUE(foldIn(kHeading, kBlock));
    EXPECT_EQ(nextWrite(), RoadmapWrite::Result::Ok);
}

// INV-2
TEST_F(RoadmapFoldInStore, Inv2BulletsAreStoreRows) {
    migrate();
    ASSERT_TRUE(foldIn(kHeading, kBlock));
    const auto w = item(QStringLiteral("DEMO-0010"));
    ASSERT_TRUE(w);
    EXPECT_EQ(w->status, QStringLiteral("planned"));
    EXPECT_EQ(w->headline, QStringLiteral("A finding."));
    EXPECT_EQ(w->kind, QStringLiteral("audit-fix"));
    EXPECT_EQ(w->source, QStringLiteral("audit-2026-10-31"));
    EXPECT_EQ(w->lanes, QStringList{QStringLiteral("a")});
    EXPECT_EQ(w->created, QDate::currentDate().toString(Qt::ISODate));
    EXPECT_TRUE(w->body.contains(QStringLiteral("src/a.cpp:3")));
    EXPECT_TRUE(readFile(roadmap).contains("[DEMO-0010]"));
}

// INV-3
TEST_F(RoadmapFoldInStore, Inv3LaymanIsSuppliedOrKept) {
    migrate();
    ASSERT_TRUE(foldIn(kHeading, kBlock));
    const auto given = item(QStringLiteral("DEMO-0010"));
    ASSERT_TRUE(given);
    EXPECT_FALSE(given->layman.isEmpty());

    ASSERT_TRUE(foldIn(kHeading, QString::fromUtf8(
        "- \xF0\x9F\x93\x8B [DEMO-0011] **Another.**\n"
        "  **Layman:** Written by hand.\n"
        "  Kind: audit-fix.\n"
        "  Source: audit-2026-10-31.\n")));
    const auto kept = item(QStringLiteral("DEMO-0011"));
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->layman, QStringLiteral("Written by hand"));  // the render adds the stop
}

// INV-4
TEST_F(RoadmapFoldInStore, Inv4Placement) {
    migrate();
    ASSERT_TRUE(foldIn(kHeading, kBlock));
    ASSERT_TRUE(foldIn(kHeading, QString::fromUtf8(
        "### Audit fold-in\n\n"
        "- \xF0\x9F\x93\x8B [DEMO-0012] **A second finding.**\n"
        "  Kind: audit-fix.\n"
        "  Source: audit-2026-10-31.\n")));
    ASSERT_TRUE(foldIn(kHeading, QString::fromUtf8(
        "- \xF0\x9F\x93\x8B [DEMO-0013] **No heading.**\n"
        "  Kind: audit-fix.\n"
        "  Source: audit-2026-10-31.\n")));
    const QByteArray text = readFile(roadmap);
    const auto at = [&](const char *needle) { return text.indexOf(needle); };
    ASSERT_GE(at("## To Do"), 0);
    EXPECT_EQ(text.count("### Audit fold-in"), 1) << text.constData();
    EXPECT_LT(at("## To Do"), at("[DEMO-0001]"));
    EXPECT_LT(at("[DEMO-0001]"), at("[DEMO-0013]"));
    EXPECT_LT(at("[DEMO-0013]"), at("### Audit fold-in"));
    EXPECT_LT(at("### Audit fold-in"), at("[DEMO-0010]"));
    EXPECT_LT(at("[DEMO-0010]"), at("[DEMO-0012]"));
    EXPECT_LT(at("[DEMO-0012]"), at("## Later"));
    EXPECT_LT(at("## Later"), at("[DEMO-0002]"));
}

// INV-5
TEST_F(RoadmapFoldInStore, Inv5ProseIsKept) {
    migrate();
    ASSERT_TRUE(foldIn(kHeading, QString::fromUtf8(
        "### Test Audit\n\nClosed inline, nothing deferred.\n")));
    EXPECT_EQ(nextWrite(), RoadmapWrite::Result::Ok);
    const QByteArray text = readFile(roadmap);
    EXPECT_TRUE(text.contains("### Test Audit"));
    EXPECT_TRUE(text.contains("Closed inline, nothing deferred."));
}

// INV-6
TEST_F(RoadmapFoldInStore, Inv6ARefusalWritesNothing) {
    migrate();
    const QByteArray before = readFile(roadmap);
    EXPECT_FALSE(foldIn(QStringLiteral("## No Such Heading"), kBlock));
    EXPECT_EQ(readFile(roadmap), before);
    EXPECT_FALSE(item(QStringLiteral("DEMO-0010")));

    EXPECT_FALSE(foldIn(kHeading, QString::fromUtf8(
        "### Audit fold-in\n\n"
        "- \xF0\x9F\x93\x8B [DEMO-0014] **New.**\n"
        "  Kind: audit-fix.\n"
        "  Source: audit-2026-10-31.\n\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **Already held.**\n"
        "  Kind: audit-fix.\n"
        "  Source: audit-2026-10-31.\n")));
    EXPECT_EQ(readFile(roadmap), before);
    EXPECT_FALSE(item(QStringLiteral("DEMO-0014")));
}

// INV-7
TEST_F(RoadmapFoldInStore, Inv7MarkdownProjectIsSpliced) {
    ASSERT_TRUE(foldIn(kHeading, kBlock));
    const QByteArray text = readFile(roadmap);
    EXPECT_LT(text.indexOf("## To Do"), text.indexOf("[DEMO-0010]"));
    EXPECT_LT(text.indexOf("[DEMO-0010]"), text.indexOf("[DEMO-0001]"));
}
