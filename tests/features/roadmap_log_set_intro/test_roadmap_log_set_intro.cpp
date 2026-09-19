// ANTS-4949 / ANTS-4968 — op:"set_intro" and op:"set_preamble".
// Contract: tests/features/roadmap_log_set_intro/spec.md
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
        "# Wrong Project \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        "The old work intro.\n"
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


QJsonObject introReq(const QString &root, const QString &section, const QString &text) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    if (!section.isNull()) req[QStringLiteral("section")] = section;
    req[QStringLiteral("new_text")] = text;
    return req;
}

int countLinesStarting(const std::string &md, const std::string &prefix) {
    int n = 0;
    size_t at = 0;
    while (at < md.size()) {
        const size_t eol = md.find('\n', at);
        const std::string line = md.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (line.rfind(prefix, 0) == 0) ++n;
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    return n;
}

}  // namespace

TEST(RoadmapLogSetIntro, ReplacesVerbatimAndRenders) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("New work intro.\n  - an indented point\n")), false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_GT(resp.value(QStringLiteral("replaced_intro_chars")).toInt(), 0);
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_TRUE(has(md, "## Work\n\nNew work intro.\n  - an indented point\n"))
        << "the intro was not stored verbatim:\n" << md;
    EXPECT_FALSE(has(md, "The old work intro."));
}

TEST(RoadmapLogSetIntro, HeadingLineRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("work"),
                 QStringLiteral("Text.\n## Sneaky\n")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("bad_intro"));
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}

TEST(RoadmapLogSetIntro, MissingSectionNamesSetPreamble) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QString(), QStringLiteral("x")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(), QStringLiteral("missing_field"));
    EXPECT_TRUE(has(resp.value(QStringLiteral("error")).toString().toStdString(),
                    "set_preamble"))
        << "a caller who forgot `section` must not land on the preamble";
}

TEST(RoadmapLogSetIntro, UnknownSectionOffersCandidates) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QStringLiteral("wrok"), QStringLiteral("x")), false).object();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_not_found"));
    EXPECT_TRUE(resp.value(QStringLiteral("candidates")).toArray()
                    .contains(QJsonValue(QStringLiteral("work"))));
}

TEST(RoadmapLogSetIntro, DryRunWritesNothing) {
    Fx fx; ASSERT_TRUE(fx.ok());
    const QByteArray before = readAll(roadmapPath(fx.root));
    RemoteControl rc(nullptr);
    QJsonObject req = introReq(fx.root, QStringLiteral("work"), QStringLiteral("Preview."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(req, false).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(readAll(roadmapPath(fx.root)), before);
}

TEST(RoadmapLogSetPreamble, ReplacesTheTitle) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
        introReq(fx.root, QString(),
                 QString::fromUtf8("# Right Project \xE2\x80\x94 Roadmap\n\nA fresh preamble.")),
        true).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const std::string md = readAll(roadmapPath(fx.root)).toStdString();
    EXPECT_EQ(md.rfind("<!-- ants-roadmap-format: 1 -->", 0), 0u)
        << "the render must still open on the format marker";
    EXPECT_TRUE(has(md, "# Right Project \xE2\x80\x94 Roadmap\n\nA fresh preamble."));
    EXPECT_FALSE(has(md, "Wrong Project"));
    EXPECT_EQ(countLinesStarting(md, "# "), 1) << "two H1s in the published file";
    EXPECT_TRUE(has(md, "## Work")) << "the sections survived";
}

TEST(RoadmapLogSetPreamble, SecondTitleOrSubheadingRefused) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    for (const char *t : {"# One\n# Two", "# One\n## Sub"}) {
        const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
            introReq(fx.root, QString(), QString::fromLatin1(t)), true).object();
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("bad_intro")) << t;
    }
}

// ANTS-4555 — every rendered file says it is generated, once, under the
// format marker, and a preamble that already carries the notice is not given
// a second copy.
TEST(RoadmapLogSetPreamble, Ants4555GeneratedNoticeAppearsOnce) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QString notice = QStringLiteral(
        "<!-- Generated from the Ants Terminal roadmap store. Edit it with "
        "roadmap_log; hand edits are discarded by the next write. -->");
    for (const QString &text : {QStringLiteral("# Demo — Roadmap"),
                                notice + QStringLiteral("\n\n# Demo — Roadmap")}) {
        const QJsonObject resp = rc.cmdRoadmapLogSetIntroForTest(
            introReq(fx.root, QString(), text), true).object();
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << QJsonDocument(resp).toJson().toStdString();
        const QString md = QString::fromUtf8(readAll(roadmapPath(fx.root)));
        const QStringList lines = md.split(QLatin1Char('\n'));
        ASSERT_GE(lines.size(), 2);
        EXPECT_TRUE(lines.at(0).contains(QStringLiteral("ants-roadmap-format")));
        // Line 1 when the render adds it; line 2 when a stored preamble
        // already carried it after a blank line.
        const int at = lines.indexOf(notice);
        EXPECT_TRUE(at == 1 || at == 2)
            << "the notice must sit just under the marker:\n"
            << lines.mid(0, 5).join(QLatin1Char('\n')).toStdString();
        EXPECT_EQ(md.count(QStringLiteral("Generated from the Ants Terminal roadmap store")), 1)
            << md.toStdString();
    }
}
