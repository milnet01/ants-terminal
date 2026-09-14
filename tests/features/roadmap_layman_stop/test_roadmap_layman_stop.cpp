// ANTS-4955 — a Layman summary is stored without its closing full stop and
// rendered with one.
// Contract: tests/features/roadmap_layman_stop/spec.md
//
// INV-1..3 are pure: they call the render and the trailer parse directly.
// INV-4 and INV-6 migrate a small fixture into a store at
// RoadmapStore::defaultPath() (redirected into the case's sandbox). INV-5 uses
// a plain ROADMAP.md with no store, which is the markdown write path.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

QString readText(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

RoadmapStore::ItemWrite itemWith(const QString &layman) {
    RoadmapStore::ItemWrite it;
    it.id       = QStringLiteral("DEMO-0001");
    it.status   = QStringLiteral("planned");
    it.headline = QStringLiteral("A thing.");
    it.kind     = QStringLiteral("fix");
    it.source   = QStringLiteral("test");
    it.layman   = layman;
    return it;
}

// The parsed Layman value of a rendered bullet: everything after the head line.
QString reparsedLayman(const QString &rendered) {
    const QString body = rendered.mid(rendered.indexOf(QLatin1Char('\n')) + 1);
    return RoadmapParse::trailerValuesIn(body).layman.value;
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
    "magni dolores eos qui ratione voluptatem sequi nesciunt neque porro.\n";

QByteArray storeFixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n"
        "## Work\n"
        "\n"
        // Ends on a trailing trailer run, so the values live only in columns.
        "- \xF0\x9F\x93\x8B [DEMO-0003] **An open item.**\n"
        "  Some prose about it.\n"
        "  Layman: A column-only thing.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0004] **Another open item.**\n"
        "  More prose.\n"
        "  Layman: Kept as written.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "\n";
    return b;
}

struct StoreFx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    qint64 projectId = 0;
    QString root;

    bool ok() {
        if (!tmp.isValid()) return false;
        guard.setEnv("XDG_DATA_HOME",
                     QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
        const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
        if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), storeFixture()))
            return false;
        root = QFileInfo(rawRoot).canonicalFilePath();
        auto store = openStore(RoadmapStore::Access::Bulk);
        if (!store) return false;
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return false; }
        const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                                   QStringLiteral("demo"));
        RoadmapMigrateLoad::Options opts;
        opts.changedAt   = QStringLiteral("2026-09-14T10:00:00Z");
        opts.projectRoot = root;
        const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
        if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return false; }
        projectId = out.projectId;
        return true;
    }

    QString roadmap() const { return readText(root + QStringLiteral("/ROADMAP.md")); }

    std::optional<RoadmapStore::ItemWrite> item(const QString &id) const {
        auto store = openStore(RoadmapStore::Access::Interactive);
        if (!store) return std::nullopt;
        QString err;
        const auto pk = store->findItem(projectId, id, &err);
        if (!pk) { ADD_FAILURE() << "findItem: " << err.toStdString(); return std::nullopt; }
        return store->readItem(*pk, &err);
    }

    QJsonObject amend(RemoteControl &rc, const QString &id, const QString &field,
                      const QString &value) const {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("amend_field");
        req[QStringLiteral("id")]         = id;
        req[QStringLiteral("field")]      = field;
        req[QStringLiteral("value")]      = value;
        return rc.cmdRoadmapLogAmendFieldForTest(req).object();
    }
};

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(RoadmapLaymanStop, Inv1RenderEndsBareValueWithStop) {
    const QString text = RoadmapRender::bulletText(itemWith(QStringLiteral("Faster start")));
    EXPECT_TRUE(text.contains(QStringLiteral("**Layman:** Faster start.\n")))
        << text.toStdString();
}

// ---------------------------------------------------------------- INV-2 -----

TEST(RoadmapLaymanStop, Inv2RenderAddsNothingAfterBangOrQuestion) {
    const QString bang = RoadmapRender::bulletText(itemWith(QStringLiteral("Faster start!")));
    EXPECT_TRUE(bang.contains(QStringLiteral("**Layman:** Faster start!\n")))
        << bang.toStdString();
    const QString ask = RoadmapRender::bulletText(itemWith(QStringLiteral("Faster start?")));
    EXPECT_TRUE(ask.contains(QStringLiteral("**Layman:** Faster start?\n")))
        << ask.toStdString();
}

// ---------------------------------------------------------------- INV-3 -----

TEST(RoadmapLaymanStop, Inv3ParseAfterRenderIsIdentity) {
    const QString dots = RoadmapRender::bulletText(itemWith(QStringLiteral("Wait..")));
    EXPECT_TRUE(dots.contains(QStringLiteral("**Layman:** Wait...\n")))
        << "an ellipsis is stored with one dot fewer and gets it back: "
        << dots.toStdString();

    for (const QString &stored : {QStringLiteral("Faster start"),
                                  QStringLiteral("Faster start!"),
                                  QStringLiteral("Wait..")}) {
        const QString rendered = RoadmapRender::bulletText(itemWith(stored));
        EXPECT_EQ(reparsedLayman(rendered), stored) << rendered.toStdString();
    }
}

// ---------------------------------------------------------------- INV-4 -----

TEST(RoadmapLaymanStop, Inv4AmendFieldDropsOneStop) {
    StoreFx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    QJsonObject resp = fx.amend(rc, QStringLiteral("DEMO-0003"), QStringLiteral("layman"),
                                QStringLiteral("Faster start."));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    auto item = fx.item(QStringLiteral("DEMO-0003"));
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->layman, QStringLiteral("Faster start"));
    const QString md = fx.roadmap();
    EXPECT_TRUE(md.contains(QStringLiteral("**Layman:** Faster start.\n"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("Faster start.."))) << md.toStdString();

    resp = fx.amend(rc, QStringLiteral("DEMO-0003"), QStringLiteral("layman"),
                    QStringLiteral("Wait..."));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    item = fx.item(QStringLiteral("DEMO-0003"));
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->layman, QStringLiteral("Wait.."));
    EXPECT_TRUE(fx.roadmap().contains(QStringLiteral("**Layman:** Wait...\n")))
        << fx.roadmap().toStdString();
}

// ---------------------------------------------------------------- INV-5 -----

TEST(RoadmapLaymanStop, Inv5MarkdownAppendEmitsRenderedLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/ROADMAP.md"),
        "# Fresh Roadmap\n"
        "\n"
        "## Backlog\n"
        "\n"
        "- \xF0\x9F\x93\x8B [ANTS-9001] **An existing bullet.**\n"
        "  Kind: implement.\n"
        "  Source: test.\n"
        "\n"));
    ASSERT_TRUE(writeFile(tmp.path() + QStringLiteral("/.roadmap-counter"), "9001\n"));

    RemoteControl rc(nullptr);
    const auto append = [&](const QString &headline, const QString &layman) {
        QJsonObject r;
        r[QStringLiteral("caller_cwd")] = tmp.path();
        r[QStringLiteral("op")]         = QStringLiteral("append");
        r[QStringLiteral("section")]    = QStringLiteral("backlog");
        r[QStringLiteral("status")]     = QStringLiteral("planned");
        r[QStringLiteral("headline")]   = headline;
        r[QStringLiteral("kind")]       = QStringLiteral("fix");
        r[QStringLiteral("source")]     = QStringLiteral("test");
        r[QStringLiteral("layman")]     = layman;
        const QJsonObject out = rc.cmdRoadmapLogAppendForTest(r).object();
        EXPECT_TRUE(out.value(QStringLiteral("ok")).toBool())
            << QJsonDocument(out).toJson().toStdString();
    };
    append(QStringLiteral("With the stop."), QStringLiteral("Faster start."));
    append(QStringLiteral("Without the stop."), QStringLiteral("Quicker start"));
    append(QStringLiteral("With a bang."), QStringLiteral("Fastest start!"));

    const QString md = readText(tmp.path() + QStringLiteral("/ROADMAP.md"));
    EXPECT_TRUE(md.contains(QStringLiteral("  **Layman:** Faster start.\n"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("Faster start.."))) << md.toStdString();
    EXPECT_TRUE(md.contains(QStringLiteral("  **Layman:** Quicker start.\n"))) << md.toStdString();
    EXPECT_TRUE(md.contains(QStringLiteral("  **Layman:** Fastest start!\n"))) << md.toStdString();
}

// ---------------------------------------------------------------- INV-7 -----

TEST(RoadmapLaymanStop, Inv7StoreAppendDropsOneStop) {
    StoreFx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = fx.root;
    r[QStringLiteral("op")]         = QStringLiteral("append");
    r[QStringLiteral("section")]    = QStringLiteral("work");
    r[QStringLiteral("status")]     = QStringLiteral("planned");
    r[QStringLiteral("headline")]   = QStringLiteral("Appended through the store.");
    r[QStringLiteral("kind")]       = QStringLiteral("fix");
    r[QStringLiteral("source")]     = QStringLiteral("test");
    r[QStringLiteral("layman")]     = QStringLiteral("Faster start.");
    const QJsonObject out = rc.cmdRoadmapLogAppendForTest(r).object();
    ASSERT_TRUE(out.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(out).toJson().toStdString();
    const QString id = out.value(QStringLiteral("id")).toString();
    ASSERT_FALSE(id.isEmpty()) << QJsonDocument(out).toJson().toStdString();

    const auto item = fx.item(id);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->layman, QStringLiteral("Faster start"));
    const QString md = fx.roadmap();
    EXPECT_TRUE(md.contains(QStringLiteral("**Layman:** Faster start.\n"))) << md.toStdString();
    EXPECT_FALSE(md.contains(QStringLiteral("Faster start.."))) << md.toStdString();
}

// ---------------------------------------------------------------- INV-6 -----

TEST(RoadmapLaymanStop, Inv6MigratedValueRendersWithStop) {
    StoreFx fx; ASSERT_TRUE(fx.ok());
    auto item = fx.item(QStringLiteral("DEMO-0004"));
    ASSERT_TRUE(item.has_value());
    ASSERT_EQ(item->layman, QStringLiteral("Kept as written"))
        << "precondition: the import drops one trailing stop";

    // Any write re-renders the file; amend a different column of the same item.
    RemoteControl rc(nullptr);
    const QJsonObject resp = fx.amend(rc, QStringLiteral("DEMO-0004"),
                                      QStringLiteral("kind"), QStringLiteral("chore"));
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QString md = fx.roadmap();
    EXPECT_TRUE(md.contains(QStringLiteral("**Layman:** Kept as written.\n"))) << md.toStdString();
}
