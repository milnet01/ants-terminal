// ANTS-4958 — op:"delete_section" and op:"move_section".
// Contract: tests/features/roadmap_log_section_ops/spec.md
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


QJsonObject sectionReq(const QString &root, const QString &section) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("section")]    = section;
    return req;
}

size_t at(const std::string &md, const char *needle) {
    const size_t p = md.find(needle);
    EXPECT_NE(p, std::string::npos) << needle << " is not in:\n" << md;
    return p;
}

}  // namespace

TEST(RoadmapLogSectionOps, DeleteEmptySectionReturnsWhatItHeld) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogDeleteSectionForTest(sectionReq(fx.root, QStringLiteral("empty"))).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(has(resp.value(QStringLiteral("removed_intro")).toString().toStdString(),
                    "The emptied section's intro."));
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_FALSE(has(md, "## Empty"));
    EXPECT_TRUE(has(md, "## Parent")) << "a neighbour went with it";
}

TEST(RoadmapLogSectionOps, DeleteRefusedWhileAnItemIsFiled) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogDeleteSectionForTest(sectionReq(fx.root, QStringLiteral("work"))).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("section_not_empty"));
    EXPECT_EQ(resp.value(QStringLiteral("item_ids")).toArray().size(), 1);
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}

TEST(RoadmapLogSectionOps, DeleteRefusedWithSubsections) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp =
        rc.cmdRoadmapLogDeleteSectionForTest(sectionReq(fx.root, QStringLiteral("parent"))).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_has_subsections"));
}

TEST(RoadmapLogSectionOps, MoveAfterStepsPastTheAnchorsSubsections) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req = sectionReq(fx.root, QStringLiteral("later"));
    req[QStringLiteral("after_section")] = QStringLiteral("parent");
    const QJsonObject resp = rc.cmdRoadmapLogMoveSectionForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_LT(at(md, "## Work"), at(md, "## Empty"));
    EXPECT_LT(at(md, "### Child"), at(md, "## Later"))
        << "the move landed inside the anchor's subsections";
    EXPECT_LT(at(md, "## Later"), at(md, "[DEMO-0008]")) << "its item moved with it";
}

TEST(RoadmapLogSectionOps, MoveBeforeTakesSubsectionsAlong) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject req = sectionReq(fx.root, QStringLiteral("parent"));
    req[QStringLiteral("before_section")] = QStringLiteral("work");
    const QJsonObject resp = rc.cmdRoadmapLogMoveSectionForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("sections_moved")).toInt(), 2);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_LT(at(md, "## Parent"), at(md, "### Child"));
    EXPECT_LT(at(md, "### Child"), at(md, "## Work"));
    EXPECT_LT(at(md, "## Work"), at(md, "## Later"));
}

TEST(RoadmapLogSectionOps, MoveRefusals) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    struct Case { const char *section, *key, *anchor; };
    for (const Case c : {Case{"later", "before_section", "child"},   // would adopt
                         Case{"parent", "after_section", "child"}}) { // into itself
        QJsonObject req = sectionReq(fx.root, QString::fromLatin1(c.section));
        req[QString::fromLatin1(c.key)] = QString::fromLatin1(c.anchor);
        const QJsonObject resp = rc.cmdRoadmapLogMoveSectionForTest(req).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
            << c.section << " " << c.key << " " << c.anchor;
    }
    QJsonObject both = sectionReq(fx.root, QStringLiteral("later"));
    both[QStringLiteral("after_section")]  = QStringLiteral("work");
    both[QStringLiteral("before_section")] = QStringLiteral("work");
    EXPECT_EQ(rc.cmdRoadmapLogMoveSectionForTest(both).object()
                  .value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}
