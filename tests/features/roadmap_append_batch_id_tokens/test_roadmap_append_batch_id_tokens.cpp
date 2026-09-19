// ANTS-4580 — `{{id:N}}` tokens in append_batch.
// Contract: tests/features/roadmap_append_batch_id_tokens/spec.md
//
// Behavioural, through roadmap_log itself: each case migrates a small
// markdown fixture into a sandboxed store and reads the rendered file back.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
#include <optional>
#include <string>

ANTS_TEST_SCOPE();

namespace {

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL machine-global store under XDG_DATA_HOME.
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

const char *kPad =
    "Intro paragraph that exists purely to pad this fixture past the 1 KiB\n"
    "minimum-parseable-size gate the roadmap_log write paths enforce before\n"
    "they will trust an ants-v1 walk. Lorem ipsum dolor sit amet, consectetur\n"
    "adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore\n"
    "magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco\n"
    "laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in\n"
    "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla\n"
    "pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa\n"
    "qui officia deserunt mollit anim id est laborum. Sed ut perspiciatis unde\n"
    "omnis iste natus error sit voluptatem accusantium doloremque laudantium,\n"
    "totam rem aperiam, eaque ipsa quae ab illo inventore veritatis et quasi\n"
    "architecto beatae vitae dicta sunt explicabo. Nemo enim ipsam voluptatem\n"
    "quia voluptas sit aspernatur aut odit aut fugit, sed quia consequuntur\n"
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n"
    "Quisquam est, qui dolorem ipsum quia dolor sit amet, consectetur, adipisci\n"
    "velit, sed quia non numquam eius modi tempora incidunt ut labore.\n";

QByteArray fixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
        "  Layman: A thing to do.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "## Later\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0008] **A later item.**\n"
        "  Layman: A later thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "## Empty\n"
        "\n"
        "The emptied section's intro.\n"
        "\n"
        "## Parent\n"
        "\n"
        "### Child\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0009] **A child item.**\n"
        "  Layman: A child thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
        return QString();
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    *projectId = out.projectId;
    return root;
}



QString roadmapPath(const QString &root) {
    return QDir(root).filePath(QStringLiteral("ROADMAP.md"));
}

struct Fx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    qint64 projectId = 0;
    QString root;
    bool ok() {
        if (!tmp.isValid()) return false;
        root = seedMigrated(guard, tmp, &projectId);
        return !root.isEmpty();
    }
};


QJsonObject bullet(const QString &headline, const QString &body) {
    QJsonObject b;
    b[QStringLiteral("headline")] = headline;
    b[QStringLiteral("status")]   = QStringLiteral("planned");
    b[QStringLiteral("kind")]     = QStringLiteral("feature");
    b[QStringLiteral("source")]   = QStringLiteral("test");
    b[QStringLiteral("layman")]   = QStringLiteral("A plain summary.");
    b[QStringLiteral("body")]     = body;
    return b;
}

QJsonObject batchReq(const QString &root, const QJsonArray &bullets) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("section")]    = QStringLiteral("work");
    req[QStringLiteral("bullets")]    = bullets;
    return req;
}

}  // namespace

TEST(RoadmapAppendBatchIdTokens, SiblingsCiteEachOther) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendBatchForTest(batchReq(fx.root, {
        bullet(QStringLiteral("First item."), QStringLiteral("Pairs with {{id:1}}.")),
        bullet(QStringLiteral("Second item."), QStringLiteral("The other half of {{id:0}}.")),
    })).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QJsonArray ids = resp.value(QStringLiteral("ids")).toArray();
    ASSERT_EQ(ids.size(), 2);
    const std::string id0 = ids.at(0).toString().toStdString();
    const std::string id1 = ids.at(1).toString().toStdString();
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, ("Pairs with " + id1 + ".").c_str())) << md;
    EXPECT_TRUE(has(md, ("The other half of " + id0 + ".").c_str())) << md;
    EXPECT_FALSE(has(md, "{{id:")) << "a token reached the published file";
}

TEST(RoadmapAppendBatchIdTokens, TokenToASkippedBulletRefusesTheCall) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    QJsonObject broken = bullet(QStringLiteral("Skipped item."), QString());
    broken[QStringLiteral("kind")] = QStringLiteral("not-a-kind");
    const QJsonObject resp = rc.cmdRoadmapLogAppendBatchForTest(batchReq(fx.root, {
        bullet(QStringLiteral("Cites a skipped one."), QStringLiteral("See {{id:1}}.")),
        broken,
    })).object();
    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before) << "a partial batch was written";
}

TEST(RoadmapAppendBatchIdTokens, TokenPastTheBatchRefuses) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendBatchForTest(batchReq(fx.root, {
        bullet(QStringLiteral("Cites nothing real."), QStringLiteral("See {{id:7}}.")),
    })).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "does not exist"));
}
