// ANTS-4970 — a wrapped amend_body match re-wraps the line it joined.
// Contract: tests/features/roadmap_amend_body_rewrap/spec.md
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
        "  The first line of a body wrapped at about seventy columns, like\n"
        "  every roadmap body is, so that a phrase can span a line break and\n"
        "  still be matched by amend_body in a single call, which re-flows it.\n"
        "  One more ordinary line closes the paragraph off neatly here.\n"
        "  Layman: A thing to do.\n"
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


QJsonObject amendReq(const QString &root, const QString &oldText, const QString &newText) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
    req[QStringLiteral("old_text")]   = oldText;
    req[QStringLiteral("new_text")]   = newText;
    return req;
}

int longestBodyLine(const std::string &md) {
    const size_t start = md.find("[DEMO-0007]");
    const size_t end = md.find("Layman", start);
    int longest = 0, cur = 0;
    for (size_t i = md.find('\n', start) + 1; i < end; ++i) {
        if (md[i] == '\n') { longest = std::max(longest, cur); cur = 0; }
        else if ((md[i] & 0xC0) != 0x80) ++cur;
    }
    return longest;
}

}  // namespace

TEST(RoadmapAmendBodyRewrap, WrappedMatchIsRewrappedToTheBodysWidth) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const int before = longestBodyLine(readAll(roadmapPath(fx.root)).toStdString());
    ASSERT_GT(before, 60);
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(amendReq(fx.root,
        QStringLiteral("span a line break and still be matched"),
        QStringLiteral("span a line break, keep every one of its words, and still "
                       "be matched"))).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    ASSERT_TRUE(resp.value(QStringLiteral("wrapped_match")).toBool());
    EXPECT_TRUE(resp.value(QStringLiteral("rewrapped")).toBool());
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "keep every one of its words,"));
    EXPECT_LE(longestBodyLine(md), before)
        << "a line is wider than any the body had before:\n" << md;
}

TEST(RoadmapAmendBodyRewrap, SingleLineMatchIsUntouched) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAmendBodyForTest(amendReq(fx.root,
        QStringLiteral("One more ordinary line"),
        QStringLiteral("One more perfectly ordinary line"))).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(resp.contains(QStringLiteral("rewrapped")));
    EXPECT_TRUE(has(readAll(roadmapPath(fx.root)).toStdString(),
                    "  One more perfectly ordinary line closes the paragraph off neatly here.\n"));
}
