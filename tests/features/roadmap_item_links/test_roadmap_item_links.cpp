// ANTS-4079 — item links: write, guard, read and migrate roadmap relationships.
// Contract: tests/features/roadmap_item_links/spec.md, which points at
// docs/specs/ANTS-4079-item-links.md.
//
// Behavioural, through the verbs: each case migrates two small markdown
// projects into one sandboxed store and drives roadmap_log / roadmap_query.

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
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>

#include <memory>
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

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL machine-global store under XDG_DATA_HOME, which the guard redirects.
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

// The write paths refuse an ants-v1 file under 1 KiB as unparseable.
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

QByteArray item(const char *id, const char *emoji, const char *headline,
                const QByteArray &extraBody = QByteArray()) {
    QByteArray b = "- ";
    b += emoji;
    b += " [";
    b += id;
    b += "] **";
    b += headline;
    b += "**\n";
    b += extraBody;
    b += "  Layman: A thing.\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n";
    return b;
}

const char *kPlanned = "\xF0\x9F\x93\x8B";

QByteArray demoFixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Demo \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n## Work\n\n";
    b += item("DEMO-0001", kPlanned, "The parent.");
    b += item("DEMO-0002", kPlanned, "Part one.");
    b += item("DEMO-0003", kPlanned, "Part two.");
    b += item("DEMO-0004", kPlanned, "A blocker.");
    return b;
}

QByteArray vestFixture() {
    QByteArray b =
        "<!-- ants-roadmap-format: 1 -->\n"
        "\n"
        "# Vest \xE2\x80\x94 Roadmap\n"
        "\n";
    b += kPad;
    b += "\n## Work\n\n";
    b += item("VEST-0001", kPlanned, "A vest item.");
    return b;
}

QString seedProject(const QTemporaryDir &tmp, const char *dir, const QByteArray &md,
                    const QString &name, const QString &slug) {
    const QString rawRoot = QDir(tmp.path()).filePath(QString::fromLatin1(dir));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), md))
        return QString();
    QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan = RoadmapMigrate::planFrom(*disc, name, slug);
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-29T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    return root;
}

struct Fx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    QString root, vestRoot;
    bool ok() {
        if (!tmp.isValid()) return false;
        guard.setEnv("XDG_DATA_HOME",
                     QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
        root = seedProject(tmp, "demo", demoFixture(), QStringLiteral("Demo"),
                           QStringLiteral("demo"));
        vestRoot = seedProject(tmp, "vest", vestFixture(), QStringLiteral("Vest"),
                               QStringLiteral("vest"));
        return !root.isEmpty() && !vestRoot.isEmpty();
    }
    QString roadmap() const { return QDir(root).filePath(QStringLiteral("ROADMAP.md")); }
};

QJsonObject link(RemoteControl &rc, const QString &root, const char *op, const char *id,
                 const char *type, const QStringList &targets, bool dryRun = false) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QString::fromLatin1(op);
    req[QStringLiteral("id")]         = QString::fromLatin1(id);
    req[QStringLiteral("type")]       = QString::fromLatin1(type);
    req[QStringLiteral("targets")]    = QJsonArray::fromStringList(targets);
    if (dryRun) req[QStringLiteral("dry_run")] = true;
    return rc.cmdRoadmapLog(req).object();
}

QJsonObject fetch(RemoteControl &rc, const QString &root, const char *id) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("id")]         = QString::fromLatin1(id);
    const QJsonObject resp = rc.cmdRoadmapQueryForTest(req).object();
    const QJsonArray bullets = resp.value(QStringLiteral("bullets")).toArray();
    if (bullets.size() != 1) {
        ADD_FAILURE() << "fetch " << id << ": " << dump(resp);
        return {};
    }
    return bullets.first().toObject();
}

QStringList linkList(const QJsonObject &bullet, const char *key) {
    QStringList out;
    const QJsonArray a = bullet.value(QStringLiteral("links")).toObject()
                             .value(QString::fromLatin1(key)).toArray();
    for (const auto &v : a) out << v.toString();
    return out;
}

}  // namespace

// INV-1 — write, forward read, reverse read, unlink, and the idempotent repeats.
TEST(RoadmapItemLinks, LinkIsReadBothWaysAndUnlinkRemovesIt) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    QJsonObject r = link(rc, fx.root, "link", "DEMO-0002", "splits-from", {"DEMO-0001"});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(r.value(QStringLiteral("linked")).toArray().size(), 1) << dump(r);

    EXPECT_EQ(linkList(fetch(rc, fx.root, "DEMO-0002"), "splits_from"),
              QStringList{QStringLiteral("DEMO-0001")});
    EXPECT_EQ(linkList(fetch(rc, fx.root, "DEMO-0001"), "parts"),
              QStringList{QStringLiteral("DEMO-0002")}) << "reverse read";

    r = link(rc, fx.root, "link", "DEMO-0002", "splits-from", {"DEMO-0001"});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(r.value(QStringLiteral("unchanged")).toArray().size(), 1) << dump(r);
    EXPECT_TRUE(r.value(QStringLiteral("linked")).toArray().isEmpty()) << dump(r);

    r = link(rc, fx.root, "unlink", "DEMO-0002", "splits-from", {"DEMO-0001"});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(r.value(QStringLiteral("unlinked")).toArray().size(), 1) << dump(r);
    EXPECT_FALSE(fetch(rc, fx.root, "DEMO-0001").contains(QStringLiteral("links")))
        << "an item with no links omits the key";

    r = link(rc, fx.root, "unlink", "DEMO-0002", "splits-from", {"DEMO-0001"});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(r.value(QStringLiteral("unchanged")).toArray().size(), 1) << dump(r);
}

// INV-2 — an unfiled id of THIS project refuses; one of another registered
// project is stored as a cross-project row, read back under its own id.
TEST(RoadmapItemLinks, UnfiledTargetsSplitByProject) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QByteArray before = readAll(fx.roadmap());

    QJsonObject r = link(rc, fx.root, "link", "DEMO-0002", "blocked-by", {"DEMO-0999"});
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(),
              QStringLiteral("link_target_not_found")) << dump(r);
    EXPECT_TRUE(r.value(QStringLiteral("error")).toString().contains(QStringLiteral("DEMO-0999")))
        << dump(r);
    EXPECT_FALSE(fetch(rc, fx.root, "DEMO-0002").contains(QStringLiteral("links")));
    EXPECT_EQ(readAll(fx.roadmap()), before);

    r = link(rc, fx.root, "link", "DEMO-0002", "blocked-by", {"VEST-0040"});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(linkList(fetch(rc, fx.root, "DEMO-0002"), "blocked_by"),
              QStringList{QStringLiteral("VEST-0040")});

    // A filed item in the other project is an ordinary row, read both ways.
    r = link(rc, fx.root, "link", "DEMO-0003", "blocked-by", {"VEST-0001"});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(linkList(fetch(rc, fx.vestRoot, "VEST-0001"), "blocks"),
              QStringList{QStringLiteral("DEMO-0003")});
}

// INV-3 — a same-type cycle refuses and names it; another type does not.
TEST(RoadmapItemLinks, SameTypeCycleRefusesOtherTypeDoesNot) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);

    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0001", "blocked-by", {"DEMO-0002"})
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0002", "blocked-by", {"DEMO-0003"})
                    .value(QStringLiteral("ok")).toBool());

    const QJsonObject r =
        link(rc, fx.root, "link", "DEMO-0003", "blocked-by", {"DEMO-0001"});
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("link_cycle"))
        << dump(r);
    QStringList cycle;
    for (const auto &v : r.value(QStringLiteral("cycle")).toArray()) cycle << v.toString();
    for (const char *id : {"DEMO-0001", "DEMO-0002", "DEMO-0003"})
        EXPECT_TRUE(cycle.contains(QString::fromLatin1(id))) << id << " " << dump(r);
    EXPECT_TRUE(linkList(fetch(rc, fx.root, "DEMO-0003"), "blocked_by").isEmpty());

    const QJsonObject other =
        link(rc, fx.root, "link", "DEMO-0003", "supersedes", {"DEMO-0001"});
    EXPECT_TRUE(other.value(QStringLiteral("ok")).toBool()) << dump(other);
}

// § 2.2's argument refusals.
TEST(RoadmapItemLinks, LinkArgumentRefusals) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QByteArray before = readAll(fx.roadmap());

    EXPECT_EQ(link(rc, fx.root, "link", "DEMO-0001", "blocked-by", {"DEMO-0001"})
                  .value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
        << "self-link";
    for (const char *type : {"relates-to", "specified-by", "parent-of"})
        EXPECT_EQ(link(rc, fx.root, "link", "DEMO-0001", type, {"DEMO-0002"})
                      .value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
            << type;
    EXPECT_EQ(link(rc, fx.root, "link", "DEMO-0001", "blocked-by", {})
                  .value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"))
        << "empty targets";
    EXPECT_EQ(link(rc, fx.root, "link", "DEMO-0999", "blocked-by", {"DEMO-0001"})
                  .value(QStringLiteral("code")).toString(), QStringLiteral("bullet_not_found"))
        << "unknown source";

    const QJsonObject dry =
        link(rc, fx.root, "link", "DEMO-0001", "blocked-by", {"DEMO-0002"}, true);
    EXPECT_TRUE(dry.value(QStringLiteral("ok")).toBool()) << dump(dry);
    EXPECT_FALSE(fetch(rc, fx.root, "DEMO-0001").contains(QStringLiteral("links")))
        << "dry_run wrote";
    EXPECT_EQ(readAll(fx.roadmap()), before);
}
