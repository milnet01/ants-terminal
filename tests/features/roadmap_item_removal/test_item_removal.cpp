// ANTS-4487 INV-8 — roadmap_log refuses an exact duplicate append unless forced.
// Contract: tests/features/roadmap_item_removal/spec.md

#include <gtest/gtest.h>

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"
#include "../../_support/xdg_guard.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

namespace {

const char *kDupHeadline = "The widget cache grows without bound during long sessions.";

bool writeText(const QString &path, const QString &text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    return f.write(text.toUtf8()) >= 0;
}
QString readText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

// An unmigrated project: roadmap_log's markdown path.
bool markdownProject(const QString &dir) {
    return writeText(dir + QStringLiteral("/ROADMAP.md"), QString::fromUtf8(
               "# Test Roadmap\n\n## Performance\n\n"
               "- \xF0\x9F\x93\x8B [ANTS-9001] **The widget cache grows without "
               "bound during long sessions.**\n  Kind: implement.\n  Source: test.\n"))
        && writeText(dir + QStringLiteral("/.roadmap-counter"), QStringLiteral("9100\n"));
}

QJsonObject appendReq(const QString &dir, const QString &section, const QString &headline) {
    QJsonObject r;
    r["caller_cwd"] = dir;
    r["op"]         = QStringLiteral("append");
    r["section"]    = section;
    r["status"]     = QStringLiteral("planned");
    r["headline"]   = headline;
    r["kind"]       = QStringLiteral("implement");
    r["source"]     = QStringLiteral("test");
    r["layman"]     = QStringLiteral("A thing.");
    return r;
}

}  // namespace

// Markdown path: refuse, write nothing, dry run too; force files it.
TEST(RoadmapItemRemoval, appendRefusesExactDuplicate) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(markdownProject(dir.path()));
    const QString before = readText(dir.filePath(QStringLiteral("ROADMAP.md")));
    RemoteControl rc(nullptr);

    QJsonObject req = appendReq(dir.path(), QStringLiteral("performance"),
                                QString::fromUtf8(kDupHeadline));
    req["dry_run"] = true;
    const QJsonObject dry = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_EQ(dry.value(QStringLiteral("code")).toString().toStdString(),
              std::string("duplicate_item")) << "a dry run refuses where the real run would";

    req.remove(QStringLiteral("dry_run"));
    const QJsonObject refused = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_FALSE(refused.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(refused.value(QStringLiteral("code")).toString().toStdString(),
              std::string("duplicate_item"));
    EXPECT_EQ(refused.value(QStringLiteral("duplicate_of")).toString().toStdString(),
              std::string("ANTS-9001"));
    EXPECT_EQ(readText(dir.filePath(QStringLiteral("ROADMAP.md"))), before)
        << "a refused append wrote the file";
    EXPECT_EQ(readText(dir.filePath(QStringLiteral(".roadmap-counter"))).trimmed(),
              QStringLiteral("9100")) << "a refused append advanced the counter";

    req["force"] = true;
    const QJsonObject forced = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(forced.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(forced).toJson().toStdString();
    EXPECT_TRUE(readText(dir.filePath(QStringLiteral("ROADMAP.md")))
                    .contains(forced.value(QStringLiteral("id")).toString()));
}

// append_batch skips the duplicate entry and applies the rest.
TEST(RoadmapItemRemoval, appendBatchSkipsExactDuplicate) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(markdownProject(dir.path()));
    RemoteControl rc(nullptr);

    auto bullet = [](const QString &hl) {
        QJsonObject b;
        b["headline"] = hl;
        b["status"]   = QStringLiteral("planned");
        b["kind"]     = QStringLiteral("implement");
        b["source"]   = QStringLiteral("test");
        return b;
    };
    QJsonObject req;
    req["caller_cwd"] = dir.path();
    req["op"]         = QStringLiteral("append_batch");
    req["section"]    = QStringLiteral("performance");
    req["bullets"]    = QJsonArray{bullet(QString::fromUtf8(kDupHeadline)),
                                   bullet(QStringLiteral("A genuinely novel exporter."))};
    const QJsonObject out = rc.cmdRoadmapLogAppendBatchForTest(req).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out.value(QStringLiteral("applied_count")).toInt(), 1);
    const QJsonArray skipped = out.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString().toStdString(),
              std::string("duplicate_item"));
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("bullet_index")).toInt(), 0);
}

// Store path: a migrated project refuses the same way.
TEST(RoadmapItemRemoval, appendRefusesExactDuplicateOnTheStorePath) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ants_test::XdgGuard xdg;
    xdg.setEnv("XDG_DATA_HOME", dir.filePath(QStringLiteral("xdg")).toLocal8Bit());

    const QString root = dir.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(QDir().mkpath(root));
    ASSERT_TRUE(writeText(root + QStringLiteral("/ROADMAP.md"), QString::fromUtf8(
        "<!-- ants-roadmap-format: 1 -->\n\n# Demo — Roadmap\n\n## Work\n\n"
        "- \xF0\x9F\x93\x8B [DEMO-0001] **The widget cache grows without bound "
        "during long sessions.**\n  Layman: A thing.\n  Kind: implement.\n  Source: test.\n")));
    {
        const QString dbPath = RoadmapStore::defaultPath();
        QDir().mkpath(QFileInfo(dbPath).path());
        RoadmapStore store(dbPath, RoadmapStore::kDefaultHistoryCapBytes,
                           RoadmapStore::Access::Bulk);
        QString err;
        ASSERT_TRUE(store.open(&err)) << err.toStdString();
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        ASSERT_TRUE(disc.has_value()) << err.toStdString();
        RoadmapMigrateLoad::Options opts;
        opts.changedAt   = QStringLiteral("2026-09-27T10:00:00Z");
        opts.projectRoot = root;
        ASSERT_TRUE(RoadmapMigrateLoad::load(
            store, RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo")),
            opts).ok);
    }

    RemoteControl rc(nullptr);
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(
        appendReq(root, QStringLiteral("work"), QString::fromUtf8(kDupHeadline))).object();
    EXPECT_EQ(out.value(QStringLiteral("code")).toString().toStdString(),
              std::string("duplicate_item")) << QJsonDocument(out).toJson().toStdString();
    EXPECT_EQ(out.value(QStringLiteral("duplicate_of")).toString().toStdString(),
              std::string("DEMO-0001"));
}
