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

// Migrate (or re-migrate) the ROADMAP.md at `root` into the sandboxed store.
bool migrate(const QString &root, const QString &name, const QString &slug) {
    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return false;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return false; }
    const auto plan = RoadmapMigrate::planFrom(*disc, name, slug);
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-29T11:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return false; }
    return true;
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

namespace {

QJsonObject flip(RemoteControl &rc, const QString &root, const char *id, const char *to) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("id")]         = QString::fromLatin1(id);
    req[QStringLiteral("to_status")]  = QString::fromLatin1(to);
    return rc.cmdRoadmapLog(req).object();
}

// roadmap_query reports status as the emoji; the words read better in asserts.
QString statusOf(RemoteControl &rc, const QString &root, const char *id) {
    QString emoji = fetch(rc, root, id).value(QStringLiteral("status")).toString();
    if (emoji == QString::fromUtf8("\xF0\x9F\x93\x8B")) return QStringLiteral("planned");
    if (emoji == QString::fromUtf8("\xF0\x9F\x9A\xA7")) return QStringLiteral("in-progress");
    if (emoji == QString::fromUtf8("\xE2\x9C\x85"))     return QStringLiteral("shipped");
    if (emoji == QString::fromUtf8("\xF0\x9F\x9A\xAB")) return QStringLiteral("dropped");
    return emoji;
}

}  // namespace

// INV-4 — a split parent cannot ship while a part is open; dropped counts as closed.
TEST(RoadmapItemLinks, SplitParentCannotShipWithAnOpenPart) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0002", "splits-from", {"DEMO-0001"})
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0003", "splits-from", {"DEMO-0001"})
                    .value(QStringLiteral("ok")).toBool());
    const QString before = statusOf(rc, fx.root, "DEMO-0001");

    QJsonObject r = flip(rc, fx.root, "DEMO-0001", "shipped");
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("open_parts")) << dump(r);
    const QJsonArray parts = r.value(QStringLiteral("parts")).toArray();
    ASSERT_EQ(parts.size(), 2) << dump(r);
    EXPECT_EQ(parts.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("DEMO-0002"));
    EXPECT_EQ(parts.at(0).toObject().value(QStringLiteral("status")).toString(),
              QString::fromUtf8(kPlanned)) << "the emoji, as from_status carries";
    EXPECT_EQ(statusOf(rc, fx.root, "DEMO-0001"), before) << "the refusal wrote";

    // In-progress is not guarded; only shipping is.
    EXPECT_TRUE(flip(rc, fx.root, "DEMO-0001", "in-progress")
                    .value(QStringLiteral("ok")).toBool());

    ASSERT_TRUE(flip(rc, fx.root, "DEMO-0002", "shipped").value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(flip(rc, fx.root, "DEMO-0003", "dropped").value(QStringLiteral("ok")).toBool());
    r = flip(rc, fx.root, "DEMO-0001", "shipped");
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool()) << "dropped counted as open: " << dump(r);
}

// INV-4 — in flip_batch the refusal is per locator, and a part shipping in the
// same batch counts as shipped.
TEST(RoadmapItemLinks, FlipBatchRefusesPerLocator) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0002", "splits-from", {"DEMO-0001"})
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0004", "splits-from", {"DEMO-0003"})
                    .value(QStringLiteral("ok")).toBool());

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("locators")]   = QJsonArray{
        QJsonObject{{"id", "DEMO-0001"}},                 // part 0002 stays open
        QJsonObject{{"id", "DEMO-0003"}},                 // part 0004 ships too
        QJsonObject{{"id", "DEMO-0004"}}};
    const QJsonObject r = rc.cmdRoadmapLog(req).object();
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    const QJsonArray skipped = r.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1) << dump(r);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("open_parts")) << dump(r);
    EXPECT_EQ(statusOf(rc, fx.root, "DEMO-0001"), QStringLiteral("planned"));
    EXPECT_EQ(statusOf(rc, fx.root, "DEMO-0003"), QStringLiteral("shipped"));
    EXPECT_EQ(statusOf(rc, fx.root, "DEMO-0004"), QStringLiteral("shipped"));
}

// INV-5 — an open blocker warns and never refuses.
TEST(RoadmapItemLinks, OpenBlockerWarnsButDoesNotRefuse) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0001", "blocked-by", {"DEMO-0004"})
                    .value(QStringLiteral("ok")).toBool());

    const QJsonObject r = flip(rc, fx.root, "DEMO-0001", "in-progress");
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    QStringList open;
    for (const auto &v : r.value(QStringLiteral("blocked_by_open")).toArray())
        open << v.toString();
    EXPECT_EQ(open, QStringList{QStringLiteral("DEMO-0004")}) << dump(r);

    ASSERT_TRUE(flip(rc, fx.root, "DEMO-0004", "shipped").value(QStringLiteral("ok")).toBool());
    const QJsonObject after = flip(rc, fx.root, "DEMO-0001", "shipped");
    ASSERT_TRUE(after.value(QStringLiteral("ok")).toBool()) << dump(after);
    EXPECT_FALSE(after.contains(QStringLiteral("blocked_by_open"))) << dump(after);
}

// INV-10 — feedback_query reports a cited parent's parts; mapped_id_status keeps
// the parent's own status. Feedback files cite ANTS- ids only (FeedbackFile's
// id pattern), so this case seeds a third project under that prefix.
TEST(RoadmapItemLinks, FeedbackQueryReportsPartsOfACitedParent) {
    Fx fx; ASSERT_TRUE(fx.ok());
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n\n# Ants \xE2\x80\x94 Roadmap\n\n";
    md += kPad;
    md += "\n## Work\n\n";
    md += item("ANTS-0001", kPlanned, "The parent.");
    md += item("ANTS-0002", kPlanned, "Part one.");
    md += item("ANTS-0003", kPlanned, "Part two.");
    md += item("ANTS-0004", kPlanned, "Unsplit.");
    const QString ants = seedProject(fx.tmp, "ants", md, QStringLiteral("Ants"),
                                     QStringLiteral("ants"));
    ASSERT_FALSE(ants.isEmpty());
    RemoteControl rc(nullptr);
    ASSERT_TRUE(link(rc, ants, "link", "ANTS-0002", "splits-from", {"ANTS-0001"})
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(flip(rc, ants, "ANTS-0003", "shipped").value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(link(rc, ants, "link", "ANTS-0003", "splits-from", {"ANTS-0001"})
                    .value(QStringLiteral("ok")).toBool());

    const QString fb = QDir(fx.tmp.path()).filePath(QStringLiteral("Demo_Ants_MCP_Feedback.md"));
    ASSERT_TRUE(writeFile(fb,
        "<!-- ants-mcp-feedback: 2 -->\n"
        "# Ants MCP Feedback \xE2\x80\x94 Demo\n\n"
        "## 2026-09-29 \xE2\x80\x94 s\n\n"
        "### Finding A\n\n- **What:** a.\n- **Proposed ID:** ANTS-0001\n\n"
        "### Finding B\n\n- **What:** b.\n- **Proposed ID:** ANTS-0004\n"));
    QJsonObject req;
    req[QStringLiteral("path")]       = fb;
    req[QStringLiteral("caller_cwd")] = ants;
    const QJsonObject env = rc.cmdFeedbackQuery(req).object();
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool()) << dump(env);

    const QJsonObject parts = env.value(QStringLiteral("mapped_id_parts")).toObject();
    const QJsonArray p1 = parts.value(QStringLiteral("ANTS-0001")).toArray();
    ASSERT_EQ(p1.size(), 2) << dump(env);
    EXPECT_EQ(p1.at(0).toObject().value(QStringLiteral("id")).toString(),
              QStringLiteral("ANTS-0002"));
    EXPECT_EQ(p1.at(0).toObject().value(QStringLiteral("status")).toString(),
              QString::fromUtf8(kPlanned));
    EXPECT_EQ(p1.at(1).toObject().value(QStringLiteral("status")).toString(),
              QString::fromUtf8("\xE2\x9C\x85"));
    EXPECT_FALSE(parts.contains(QStringLiteral("ANTS-0004"))) << "an id with no parts";

    bool sawParent = false;
    for (const auto &v : env.value(QStringLiteral("mapped_id_status")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("id")).toString() != QLatin1String("ANTS-0001"))
            continue;
        sawParent = true;
        EXPECT_EQ(o.value(QStringLiteral("status")).toString(), QString::fromUtf8(kPlanned))
            << "mapped_id_status keeps the parent's own status";
    }
    EXPECT_TRUE(sawParent) << dump(env);
}

namespace {

QJsonObject render(RemoteControl &rc, const QString &root) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("render");
    return rc.cmdRoadmapLog(req).object();
}

// Every id's `links` object, keyed by id, so two states compare in one EXPECT.
QJsonObject allLinks(RemoteControl &rc, const QString &root, const QStringList &ids) {
    QJsonObject out;
    for (const QString &id : ids)
        out.insert(id, fetch(rc, root, id.toLatin1().constData())
                           .value(QStringLiteral("links")).toObject());
    return out;
}

int occurrences(const QByteArray &hay, const char *needle) {
    int n = 0;
    for (qsizetype at = hay.indexOf(needle); at >= 0; at = hay.indexOf(needle, at + 1)) ++n;
    return n;
}

}  // namespace

// INV-6 (ants-v1) — render then re-import restores the same rows and the same
// file: all four authored types, a cross-project target, an unresolved id and a
// same-type cycle. A prose line that is not an id list stays prose (the § 2.5
// amendment): it is neither a row nor moved.
TEST(RoadmapItemLinks, TrailersRoundTripOnAntsV1) {
    Fx fx; ASSERT_TRUE(fx.tmp.isValid());
    fx.guard.setEnv("XDG_DATA_HOME",
                    QDir(fx.tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    fx.vestRoot = seedProject(fx.tmp, "vest", vestFixture(), QStringLiteral("Vest"),
                              QStringLiteral("vest"));
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n\n# Demo \xE2\x80\x94 Roadmap\n\n";
    md += kPad;
    md += "\n## Work\n\n";
    md += item("DEMO-0001", kPlanned, "The parent.", "  Supersedes: DEMO-0003.\n");
    md += item("DEMO-0002", kPlanned, "Part one.",
               "  Blocked-by: nothing, it stands alone.\n");
    md += item("DEMO-0003", kPlanned, "Part two.", "  Supersedes: DEMO-0001.\n");
    md += item("DEMO-0004", kPlanned, "A blocker.", "  Blocked-by: DEMO-0999.\n");
    fx.root = seedProject(fx.tmp, "demo", md, QStringLiteral("Demo"), QStringLiteral("demo"));
    ASSERT_FALSE(fx.root.isEmpty());
    ASSERT_FALSE(fx.vestRoot.isEmpty());
    RemoteControl rc(nullptr);

    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0002", "splits-from", {"DEMO-0001"})
                    .value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0003", "blocked-by",
                     {"DEMO-0004", "VEST-0001", "VEST-0040"}).value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(link(rc, fx.root, "link", "DEMO-0004", "duplicate-of", {"DEMO-0002"})
                    .value(QStringLiteral("ok")).toBool());

    const QStringList ids{QStringLiteral("DEMO-0001"), QStringLiteral("DEMO-0002"),
                          QStringLiteral("DEMO-0003"), QStringLiteral("DEMO-0004")};
    const QJsonObject rowsBefore = allLinks(rc, fx.root, ids);
    EXPECT_EQ(rowsBefore.value(QStringLiteral("DEMO-0001")).toObject()
                  .value(QStringLiteral("supersedes")).toArray().size(), 1)
        << "import restores a same-type cycle: " << dump(rowsBefore);
    EXPECT_FALSE(rowsBefore.value(QStringLiteral("DEMO-0002")).toObject()
                     .contains(QStringLiteral("blocked_by")))
        << "a prose line became a row: " << dump(rowsBefore);

    ASSERT_TRUE(render(rc, fx.root).value(QStringLiteral("ok")).toBool());
    const QByteArray f1 = readAll(fx.roadmap());
    EXPECT_TRUE(f1.contains("  Blocked-by: DEMO-0004, VEST-0001, VEST-0040.")) << f1.toStdString();
    EXPECT_TRUE(f1.contains("  Blocked-by: DEMO-0999.")) << "unresolved id lost";
    EXPECT_TRUE(f1.contains("  Splits-from: DEMO-0001."));
    EXPECT_TRUE(f1.contains("  Duplicate-of: DEMO-0002."));
    // Where the author wrote it, above the trailers: lifted into a link line it
    // would render the same text, but at the bullet's end.
    EXPECT_TRUE(f1.contains("  Blocked-by: nothing, it stands alone.\n  **Layman:** A thing."))
        << "prose line moved or lost: " << f1.toStdString();
    EXPECT_EQ(occurrences(f1, "Supersedes: DEMO-"), 2) << f1.toStdString();
    // The link lines follow the trailers they were composed after.
    EXPECT_LT(f1.indexOf("Source: seed.\n  Splits-from: DEMO-0001."), f1.size());
    EXPECT_GE(f1.indexOf("Source: seed.\n  Splits-from: DEMO-0001."), 0) << f1.toStdString();

    ASSERT_TRUE(migrate(fx.root, QStringLiteral("Demo"), QStringLiteral("demo")));
    ASSERT_TRUE(render(rc, fx.root).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(readAll(fx.roadmap()), f1) << "the file moved on re-import";
    EXPECT_EQ(allLinks(rc, fx.root, ids), rowsBefore) << "the rows moved on re-import";
}

// INV-6 (pass-headings) — the `- **Key**:` form, after the Status line.
TEST(RoadmapItemLinks, TrailersRoundTripOnPassHeadings) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    QByteArray md = "# Passes\n\n";
    md += kPad;
    md += "\n## Work\n\n"
          "#### Pass 1.1 The parent\n"
          "- **Status**: todo\n"
          "- **Finding**: one.\n\n"
          "#### Pass 1.2 A part\n"
          "- **Status**: todo\n"
          "- **Splits-from**: PASS-1-1\n"
          "- **Finding**: two.\n\n"
          "#### Pass 1.3 Blocked\n"
          "- **Status**: todo\n"
          "- **Blocked-by**: PASS-1-1, PASS-9-9\n"
          "- **Finding**: three.\n";
    const QString root = seedProject(tmp, "passes", md, QStringLiteral("Passes"),
                                     QStringLiteral("passes"));
    ASSERT_FALSE(root.isEmpty());
    RemoteControl rc(nullptr);
    const QStringList ids{QStringLiteral("PASS-1-1"), QStringLiteral("PASS-1-2"),
                          QStringLiteral("PASS-1-3")};
    const QJsonObject rows = allLinks(rc, root, ids);
    EXPECT_EQ(rows.value(QStringLiteral("PASS-1-1")).toObject()
                  .value(QStringLiteral("parts")).toArray().size(), 1) << dump(rows);
    EXPECT_EQ(rows.value(QStringLiteral("PASS-1-3")).toObject()
                  .value(QStringLiteral("blocked_by")).toArray().size(), 1) << dump(rows);

    const QJsonObject r = render(rc, root);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    const QByteArray f1 = readAll(QDir(root).filePath(QStringLiteral("ROADMAP.md")));
    EXPECT_TRUE(f1.contains("- **Status**: todo\n- **Splits-from**: PASS-1-1\n")) << f1.toStdString();
    EXPECT_TRUE(f1.contains("- **Blocked-by**: PASS-1-1, PASS-9-9\n")) << "unresolved id lost";
    EXPECT_EQ(occurrences(f1, "**Blocked-by**"), 1) << f1.toStdString();

    ASSERT_TRUE(migrate(root, QStringLiteral("Passes"), QStringLiteral("passes")));
    ASSERT_TRUE(render(rc, root).value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(readAll(QDir(root).filePath(QStringLiteral("ROADMAP.md"))), f1);
    EXPECT_EQ(allLinks(rc, root, ids), rows);
}

// INV-9 — a body write declaring a link line refuses body_shadowed; prose that
// is not an id list does not.
TEST(RoadmapItemLinks, BodyWriteDeclaringALinkLineRefuses) {
    Fx fx; ASSERT_TRUE(fx.ok());
    RemoteControl rc(nullptr);
    const QByteArray before = readAll(fx.roadmap());

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = fx.root;
    req[QStringLiteral("op")]         = QStringLiteral("set_body");
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0001");
    req[QStringLiteral("new_text")]   = QStringLiteral("Some prose.\nBlocked-by: DEMO-0004.");
    QJsonObject r = rc.cmdRoadmapLog(req).object();
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("body_shadowed")) << dump(r);
    EXPECT_TRUE(r.value(QStringLiteral("error")).toString().contains(QStringLiteral("op:\"link\"")))
        << dump(r);
    EXPECT_EQ(readAll(fx.roadmap()), before);

    QJsonObject note;
    note[QStringLiteral("caller_cwd")] = fx.root;
    note[QStringLiteral("op")]         = QStringLiteral("annotate");
    note[QStringLiteral("id")]         = QStringLiteral("DEMO-0001");
    note[QStringLiteral("note")]       = QStringLiteral("**Splits-from:** DEMO-0002");
    r = rc.cmdRoadmapLog(note).object();
    EXPECT_EQ(r.value(QStringLiteral("code")).toString(), QStringLiteral("body_shadowed")) << dump(r);

    req[QStringLiteral("new_text")] = QStringLiteral("Blocked-by: nothing yet, it can start.");
    r = rc.cmdRoadmapLog(req).object();
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool()) << "prose refused: " << dump(r);
}

namespace {

// DEMO-0001 depends on DEMO-0002 and on a value no store holds, and names a
// spec; DEMO-0002 and DEMO-0003 declare each other.
QByteArray convertedFixture() {
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n\n# Demo \xE2\x80\x94 Roadmap\n\n";
    md += kPad;
    md += "\n## Work\n\n";
    md += item("DEMO-0001", kPlanned, "The parent.",
               "  Dependencies: DEMO-0002, not-an-id.\n"
               "  Spec: `docs/specs/X.md` (accepted).\n");
    md += item("DEMO-0002", kPlanned, "Part one.", "  Dependencies: DEMO-0003.\n");
    md += item("DEMO-0003", kPlanned, "Part two.", "  Dependencies: DEMO-0002.\n");
    md += item("DEMO-0004", kPlanned, "A blocker.", "  Dependencies: none.\n");
    return md;
}

QJsonObject amend(RemoteControl &rc, const QString &root, const char *id,
                  const char *oldText, const char *newText) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("amend_body");
    req[QStringLiteral("id")]         = QString::fromLatin1(id);
    req[QStringLiteral("old_text")]   = QString::fromUtf8(oldText);
    req[QStringLiteral("new_text")]   = QString::fromUtf8(newText);
    return rc.cmdRoadmapLog(req).object();
}

QJsonObject extrasOf(const QString &root, const char *id) {
    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return {};
    const auto project = store->readProjectByRoot(root);
    if (!project) { ADD_FAILURE() << "no project for " << root.toStdString(); return {}; }
    const auto pk = store->findItem(project->projectId, QString::fromLatin1(id));
    if (!pk) { ADD_FAILURE() << "no item " << id; return {}; }
    return store->readItem(*pk)->extras;
}

}  // namespace

// INV-7 — migration converts Dependencies: and Spec: into rows, keeps an
// unresolvable value in extras, and leaves the rendered body as written.
TEST(RoadmapItemLinks, MigrationConvertsDependenciesAndSpec) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString root = seedProject(tmp, "demo", convertedFixture(), QStringLiteral("Demo"),
                                     QStringLiteral("demo"));
    ASSERT_FALSE(root.isEmpty());
    RemoteControl rc(nullptr);

    const QJsonObject l1 = fetch(rc, root, "DEMO-0001").value(QStringLiteral("links")).toObject();
    EXPECT_EQ(l1.value(QStringLiteral("relates_to")).toArray(), QJsonArray{"DEMO-0002"}) << dump(l1);
    EXPECT_EQ(l1.value(QStringLiteral("specified_by")).toArray(), QJsonArray{"docs/specs/X.md"})
        << dump(l1);
    const QJsonObject l2 = fetch(rc, root, "DEMO-0002").value(QStringLiteral("links")).toObject();
    EXPECT_EQ(l2.value(QStringLiteral("relates_to")).toArray().size(), 2)
        << "one row per pair, read from both ends: " << dump(l2);
    EXPECT_FALSE(fetch(rc, root, "DEMO-0004").contains(QStringLiteral("links")))
        << "`none` is not a dependency";

    const QJsonObject ex = extrasOf(root, "DEMO-0001");
    EXPECT_EQ(ex.value(QStringLiteral("unconverted_dependencies")).toArray(),
              QJsonArray{"not-an-id"}) << dump(ex);
    EXPECT_FALSE(extrasOf(root, "DEMO-0004").contains(QStringLiteral("unconverted_dependencies")));

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("render");
    ASSERT_TRUE(rc.cmdRoadmapLog(req).object().value(QStringLiteral("ok")).toBool());
    const QByteArray md = readAll(QDir(root).filePath(QStringLiteral("ROADMAP.md")));
    EXPECT_EQ(occurrences(md, "  Dependencies: DEMO-0002, not-an-id.\n"), 1) << md.toStdString();
    EXPECT_EQ(occurrences(md, "  Spec: `docs/specs/X.md` (accepted).\n"), 1) << md.toStdString();
    EXPECT_EQ(occurrences(md, "Relates-to"), 0) << "the render composed a converted type";
}

// INV-8 — a body write re-derives relates-to: a row goes when neither endpoint
// declares it, and stays while the other endpoint still does.
TEST(RoadmapItemLinks, BodyWriteRederivesRelatesTo) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString root = seedProject(tmp, "demo", convertedFixture(), QStringLiteral("Demo"),
                                     QStringLiteral("demo"));
    ASSERT_FALSE(root.isEmpty());
    RemoteControl rc(nullptr);
    const auto relates = [&](const char *id) {
        QStringList out;
        for (const auto &v : fetch(rc, root, id).value(QStringLiteral("links")).toObject()
                                 .value(QStringLiteral("relates_to")).toArray())
            out << v.toString();
        return out;
    };

    QJsonObject r = amend(rc, root, "DEMO-0001", "DEMO-0002, not-an-id.", "not-an-id.");
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_TRUE(relates("DEMO-0001").isEmpty()) << "the row outlived its only declaration";
    EXPECT_EQ(relates("DEMO-0002"), QStringList{QStringLiteral("DEMO-0003")});

    r = amend(rc, root, "DEMO-0003", "Dependencies: DEMO-0002.", "Dependencies: none.");
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_EQ(relates("DEMO-0003"), QStringList{QStringLiteral("DEMO-0002")})
        << "DEMO-0002 still declares the pair";

    r = amend(rc, root, "DEMO-0002", "Dependencies: DEMO-0003.", "Dependencies: none.");
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << dump(r);
    EXPECT_TRUE(relates("DEMO-0003").isEmpty()) << "neither end declares it now";
}
