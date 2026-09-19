// ANTS-4977 — `dropped` roadmap items are published as 🚫.
// Contract: tests/features/roadmap_dropped_status/spec.md
//
// Behavioural. Store cases migrate a small ants-v1 fixture into a sandboxed
// store; markdown cases write a file no store knows. INV-9 (the dialog) is in
// test_roadmap_dropped_status_dialog.cpp, in the dialogs bundle.

#include "../../_support/expect.h"
#include "../../_support/xdg_guard.h"

#include "passheadingwrite.h"
#include "projectlayoutengine.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <string>

ANTS_TEST_SCOPE();

namespace {

#define kDrop QString::fromUtf8("\xF0\x9F\x9A\xAB")   // 🚫

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(body) == body.size();
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson().toStdString();
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the REAL store
// unless XDG_DATA_HOME is sandboxed first, which every Fx does.
std::unique_ptr<RoadmapStore> openStore() {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes,
        RoadmapStore::Access::Bulk);
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
    "they will trust a walk. Lorem ipsum dolor sit amet, consectetur\n"
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

const char *kDroppedLine = "- \xF0\x9F\x9A\xAB [DEMO-0009] **A dropped item.**";

QByteArray antsFixture() {
    QByteArray b = "<!-- ants-roadmap-format: 1 -->\n\n# Demo \xE2\x80\x94 Roadmap\n\n";
    b += kPad;
    b += "\n## Work\n\n"
         "- \xF0\x9F\x93\x8B [DEMO-0007] **An open item.**\n"
         "  Layman: A thing to do.\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n"
         // Closed, so it needs no Layman line (INV-11's subject).
         "- \xE2\x9C\x85 [DEMO-0008] **A shipped item.**\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n";
    b += kDroppedLine;
    b += "\n"
         "  Kind: implement.\n"
         "  Source: seed.\n"
         "\n";
    return b;
}

// A project root with antsFixture() on disk, sandboxed store, optionally
// migrated into it.
struct Fx {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    qint64 projectId = 0;
    QString root;

    bool setUp(const QByteArray &body, bool migrate) {
        if (!tmp.isValid()) return false;
        guard.setEnv("XDG_DATA_HOME",
                     QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
        const QString raw = QDir(tmp.path()).filePath(QStringLiteral("proj"));
        if (!writeFile(raw + QStringLiteral("/ROADMAP.md"), body)) return false;
        root = QFileInfo(raw).canonicalFilePath();
        if (!migrate) return true;
        auto store = openStore();
        if (!store) return false;
        QString err;
        const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
        if (!disc) { ADD_FAILURE() << err.toStdString(); return false; }
        const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                                   QStringLiteral("demo"));
        RoadmapMigrateLoad::Options opts;
        opts.changedAt   = QStringLiteral("2026-09-19T10:00:00Z");
        opts.projectRoot = root;
        const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
        if (!out.ok) { ADD_FAILURE() << out.error.toStdString(); return false; }
        projectId = out.projectId;
        return true;
    }
    QString roadmap() const { return QDir(root).filePath(QStringLiteral("ROADMAP.md")); }

    std::optional<RoadmapStore::ItemWrite> item(const QString &id) {
        auto store = openStore();
        if (!store) return std::nullopt;
        QString err;
        const auto pk = store->findItem(projectId, id, &err);
        if (!pk) return std::nullopt;
        return store->readItem(*pk, &err);
    }
};

QJsonObject flipReq(const QString &root, const QString &id, const QString &to) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("id")]         = id;
    r[QStringLiteral("to_status")]  = to;
    return r;
}

QJsonObject appendReq(const QString &root, const QString &status) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("section")]    = QStringLiteral("work");
    r[QStringLiteral("status")]     = status;
    r[QStringLiteral("headline")]   = QStringLiteral("An appended item.");
    r[QStringLiteral("kind")]       = QStringLiteral("chore");
    r[QStringLiteral("source")]     = QStringLiteral("test");
    return r;
}

QStringList idsOf(const QJsonObject &resp) {
    QStringList out;
    for (const auto &v : resp.value(QStringLiteral("bullets")).toArray())
        out << v.toObject().value(QStringLiteral("id")).toString();
    return out;
}

QJsonObject query(RemoteControl &rc, const QString &root, const QString &status) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("status")]     = status;
    return rc.cmdRoadmapQueryForTest(r).object();
}

}  // namespace

// ------------------------------------------------------------------ INV-1 --
TEST(RoadmapDroppedStatus, Inv1RenderPublishesDroppedAndDefaultLegend) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    RoadmapStore store(dir.filePath(QStringLiteral("store.db")));
    QString err;
    ASSERT_TRUE(store.open(&err)) << err.toStdString();
    const auto pid = store.registerProject(dir.path(), QStringLiteral("Demo"),
                                           QStringLiteral("demo"), &err);
    ASSERT_TRUE(pid) << err.toStdString();
    const auto rootSec = store.addSection(*pid, QStringLiteral(""), QStringLiteral(""),
                                          0, 0, std::nullopt, &err);
    ASSERT_TRUE(rootSec);
    ASSERT_TRUE(store.setSectionIntro(
        *rootSec, QStringLiteral("<!-- ants-roadmap-format: 1 -->\n\n# Demo"), &err));
    QJsonObject legend;
    legend[QStringLiteral("planned")] = QStringLiteral("Planned");
    legend[QStringLiteral("shipped")] = QStringLiteral("Shipped");
    ASSERT_TRUE(store.setLegend(*pid, legend, &err)) << err.toStdString();
    const auto sec = store.addSection(*pid, QStringLiteral("s"), QStringLiteral("S"),
                                      2, 1, std::nullopt, &err);
    ASSERT_TRUE(sec);

    auto mk = [&](const char *id, const char *status, const char *vis, int pos) {
        RoadmapStore::ItemWrite w;
        w.projectId = *pid;
        w.id = QString::fromLatin1(id);
        w.idOrigin = QStringLiteral("parsed");
        w.status = QString::fromLatin1(status);
        w.headline = QStringLiteral("Headline.");
        w.kind = QStringLiteral("implement");
        w.source = QStringLiteral("seed");
        w.layman = QStringLiteral("A plain sentence.");
        w.visibility = QString::fromLatin1(vis);
        w.sectionId = *sec;
        w.position = pos;
        return w;
    };
    ASSERT_TRUE(store.putItem(mk("D-1", "dropped", "public", 0), &err)) << err.toStdString();
    ASSERT_TRUE(store.putItem(mk("D-2", "planned", "internal", 1), &err)) << err.toStdString();

    RoadmapRender::Options o;
    o.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    const auto out = RoadmapRender::render(store, *pid, dir.path(), o, &err);
    ASSERT_TRUE(out) << err.toStdString();
    const QString text = readAll(dir.filePath(QStringLiteral("ROADMAP.md")));
    EXPECT_TRUE(text.contains(QStringLiteral("- ") + kDrop + QStringLiteral(" [D-1] **")))
        << text.toStdString();
    EXPECT_FALSE(text.contains(QStringLiteral("D-2"))) << "an internal item was published";
    EXPECT_TRUE(text.contains(QStringLiteral("- ") + kDrop
                              + QStringLiteral(" Dropped (closed, not done)")))
        << "no default legend row:\n" << text.toStdString();
}

// ------------------------------------------------------------------ INV-2 --
TEST(RoadmapDroppedStatus, Inv2MigratedDroppedBulletRoundTrips) {
    Fx fx;
    ASSERT_TRUE(fx.setUp(antsFixture(), true));
    const auto it = fx.item(QStringLiteral("DEMO-0009"));
    ASSERT_TRUE(it);
    EXPECT_EQ(it->status, QStringLiteral("dropped"));

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = fx.root;
    const QJsonObject resp = rc.cmdRoadmapLogRenderForTest(r).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const QStringList lines = readAll(fx.roadmap()).split(QLatin1Char('\n'));
    EXPECT_TRUE(lines.contains(QString::fromUtf8(kDroppedLine)))
        << readAll(fx.roadmap()).toStdString();
}

// ------------------------------------------------------------------ INV-3 --
TEST(RoadmapDroppedStatus, Inv3PassHeadingsReadAndWriteDropped) {
    EXPECT_EQ(PassHeadingWrite::passStatusKeyword(QStringLiteral("dropped")),
              QStringLiteral("dropped"));
    EXPECT_EQ(PassHeadingWrite::passStatusEmoji(QStringLiteral("dropped")), kDrop);

    for (const QString &value : {QStringLiteral("dropped"), QStringLiteral("abandoned"),
                                 QStringLiteral("wontfix"), kDrop}) {
        const QString doc = QStringLiteral("## Passes\n\n#### Pass 1.1 (LOW, S) One\n\n"
                                           "- **Status**: ") + value
            + QStringLiteral("\n\n#### Pass 1.2 (LOW, S) Two\n\n- **Status**: ")
            + value + QLatin1Char('\n');
        const auto recs = RoadmapParse::parseBullets(doc);
        ASSERT_EQ(recs.size(), 2) << value.toStdString();
        EXPECT_EQ(recs[0].status, kDrop) << value.toStdString() << " did not read as 🚫";

        RoadmapMigrate::Source s;
        s.path     = QStringLiteral("<inline>");
        s.markdown = doc;
        s.format   = RoadmapParse::detectRoadmapFormat(doc.split(QLatin1Char('\n')));
        RoadmapMigrate::Discovery d;
        d.sources.append(s);
        const auto plan = RoadmapMigrate::planFrom(d, QStringLiteral("I"), QStringLiteral("i"));
        ASSERT_EQ(plan.items.size(), 2) << value.toStdString();
        EXPECT_EQ(plan.items[0].status, QStringLiteral("dropped")) << value.toStdString();
        EXPECT_EQ(plan.items[0].provenance.value(QStringLiteral("status")).toString(),
                  QStringLiteral("asserted")) << value.toStdString();
    }
}

// ------------------------------------------------------------------ INV-4 --
TEST(RoadmapDroppedStatus, Inv4GfmFlipWritesCheckedDroppedBox) {
    QByteArray body = "# Roadmap\n\n";
    body += kPad;
    // A GFM bullet's id is its bold token (roadmap-format.md § 3.10.1).
    body += "\n## Work\n\n- [ ] **G1.** \xE2\x80\x94 a task.\n- [ ] **G2.** \xE2\x80\x94 another.\n";
    Fx fx;
    ASSERT_TRUE(fx.setUp(body, false));
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(
        flipReq(fx.root, QStringLiteral("G1"), QStringLiteral("dropped"))).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_TRUE(readAll(fx.roadmap()).contains(
        QStringLiteral("- [x] ") + kDrop + QString::fromUtf8(" **G1.** \xE2\x80\x94 a task.")))
        << readAll(fx.roadmap()).toStdString();
    EXPECT_EQ(idsOf(query(rc, fx.root, QStringLiteral("dropped"))).size(), 1)
        << "the flipped GFM bullet did not read back as 🚫";
}

// ------------------------------------------------------------------ INV-5 --
TEST(RoadmapDroppedStatus, Inv5EveryWriteOpAcceptsDropped) {
    for (const bool migrate : {true, false}) {
        SCOPED_TRACE(migrate ? "store-backed" : "markdown-backed");
        Fx fx;
        ASSERT_TRUE(fx.setUp(antsFixture(), migrate));
        // A markdown-backed append allocates from the counter file.
        if (!migrate)
            ASSERT_TRUE(writeFile(QDir(fx.root).filePath(QStringLiteral(".roadmap-counter")),
                                  "100\n"));
        RemoteControl rc(nullptr);

        const QJsonObject a = rc.cmdRoadmapLogAppendForTest(
            appendReq(fx.root, QStringLiteral("dropped"))).object();
        EXPECT_TRUE(a.value(QStringLiteral("ok")).toBool()) << dump(a);

        QJsonObject batch;
        batch[QStringLiteral("caller_cwd")] = fx.root;
        batch[QStringLiteral("section")]    = QStringLiteral("work");
        QJsonObject b = appendReq(fx.root, QStringLiteral("dropped"));
        b.remove(QStringLiteral("caller_cwd"));
        b.remove(QStringLiteral("section"));
        b[QStringLiteral("headline")] = QStringLiteral("A batch item.");
        batch[QStringLiteral("bullets")] = QJsonArray{b};
        const QJsonObject ab = rc.cmdRoadmapLogAppendBatchForTest(batch).object();
        EXPECT_TRUE(ab.value(QStringLiteral("ok")).toBool()) << dump(ab);
        EXPECT_EQ(ab.value(QStringLiteral("skipped")).toArray().size(), 0) << dump(ab);

        const QJsonObject f = rc.cmdRoadmapLogFlipForTest(
            flipReq(fx.root, QStringLiteral("DEMO-0007"), kDrop)).object();
        EXPECT_TRUE(f.value(QStringLiteral("ok")).toBool()) << dump(f);

        QJsonObject fb;
        fb[QStringLiteral("caller_cwd")] = fx.root;
        fb[QStringLiteral("to_status")]  = QStringLiteral("dropped");
        QJsonObject loc;
        loc[QStringLiteral("id")] = QStringLiteral("DEMO-0008");
        fb[QStringLiteral("locators")] = QJsonArray{loc};
        const QJsonObject fbr = rc.cmdRoadmapLogFlipBatchForTest(fb).object();
        EXPECT_TRUE(fbr.value(QStringLiteral("ok")).toBool()) << dump(fbr);

        EXPECT_EQ(idsOf(query(rc, fx.root, QStringLiteral("dropped"))).size(), 5)
            << "two appended, two flipped, one seeded";

        const QJsonObject bad = rc.cmdRoadmapLogAppendForTest(
            appendReq(fx.root, QStringLiteral("bogus"))).object();
        EXPECT_EQ(bad.value(QStringLiteral("code")).toString(), QStringLiteral("bad_status"));
        EXPECT_TRUE(bad.value(QStringLiteral("error")).toString().contains(QStringLiteral("dropped")))
            << dump(bad);
    }
}

// ------------------------------------------------------------------ INV-6 --
TEST(RoadmapDroppedStatus, Inv6ShippedToDroppedClearsTheShipDate) {
    Fx fx;
    ASSERT_TRUE(fx.setUp(antsFixture(), true));
    RemoteControl rc(nullptr);
    const QJsonObject s = rc.cmdRoadmapLogFlipForTest(
        flipReq(fx.root, QStringLiteral("DEMO-0007"), QStringLiteral("shipped"))).object();
    ASSERT_TRUE(s.value(QStringLiteral("ok")).toBool()) << dump(s);
    ASSERT_FALSE(fx.item(QStringLiteral("DEMO-0007"))->shipped.isEmpty());
    const QJsonObject d = rc.cmdRoadmapLogFlipForTest(
        flipReq(fx.root, QStringLiteral("DEMO-0007"), QStringLiteral("dropped"))).object();
    ASSERT_TRUE(d.value(QStringLiteral("ok")).toBool()) << dump(d);
    const auto it = fx.item(QStringLiteral("DEMO-0007"));
    EXPECT_EQ(it->status, QStringLiteral("dropped"));
    EXPECT_TRUE(it->shipped.isEmpty()) << "a dropped item carries a ship date";
}

// ------------------------------------------------------------------ INV-7 --
TEST(RoadmapDroppedStatus, Inv7QueryFilters) {
    Fx fx;
    ASSERT_TRUE(fx.setUp(antsFixture(), true));
    RemoteControl rc(nullptr);
    const QString id = QStringLiteral("DEMO-0009");
    EXPECT_EQ(idsOf(query(rc, fx.root, QStringLiteral("dropped"))), QStringList{id});
    EXPECT_FALSE(idsOf(query(rc, fx.root, QStringLiteral("active"))).contains(id));
    EXPECT_FALSE(idsOf(query(rc, fx.root, QStringLiteral("shipped"))).contains(id));
    EXPECT_TRUE(idsOf(query(rc, fx.root, QStringLiteral("all"))).contains(id));

    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = fx.root;
    r[QStringLiteral("mode")]       = QStringLiteral("section_index");
    const QJsonObject idx = rc.cmdRoadmapQueryForTest(r).object();
    bool found = false;
    for (const auto &v : idx.value(QStringLiteral("sections")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("slug")).toString() != QStringLiteral("work")) continue;
        found = true;
        EXPECT_EQ(o.value(QStringLiteral("total_count")).toInt(), 3) << dump(o);
        EXPECT_EQ(o.value(QStringLiteral("active_count")).toInt(), 1) << dump(o);
        EXPECT_EQ(o.value(QStringLiteral("shipped_count")).toInt(), 1) << dump(o);
    }
    EXPECT_TRUE(found) << dump(idx);
}

// ------------------------------------------------------------------ INV-8 --
TEST(RoadmapDroppedStatus, Inv8DroppedFindingCollapses) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("ROADMAP.md")),
        "# Roadmap\n\n- \xF0\x9F\x9A\xAB [ANTS-1600] **Not doing this.**\n"
        "- \xF0\x9F\x9A\xA7 [ANTS-1601] **Still open.**\n"));
    const QString fb = dir.filePath(QStringLiteral("TEST_Ants_MCP_Feedback.md"));
    ASSERT_TRUE(writeFile(fb,
        "<!-- ants-mcp-feedback: 2 -->\n# Ants MCP Feedback TEST\n\n"
        "## 2026-09-19 \xE2\x80\x94 session\n\n"
        "### Issue #1 \xE2\x80\x94 declined\n"
        "- **Proposed ID:** ANTS-1600\n- **What:** the unwanted-thing text.\n\n"
        "### Issue #2 \xE2\x80\x94 open\n"
        "- **Proposed ID:** ANTS-1601\n- **What:** the open-thing text.\n"));
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("op")]         = QStringLiteral("compact_resolved");
    req[QStringLiteral("path")]       = fb;
    req[QStringLiteral("caller_cwd")] = dir.path();
    const QJsonObject env = rc.cmdFeedbackLog(req).object();
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool()) << dump(env);
    EXPECT_EQ(env.value(QStringLiteral("findings_collapsed")).toInt(), 1) << dump(env);
    const QString md = readAll(fb);
    EXPECT_FALSE(md.contains(QStringLiteral("the unwanted-thing text")));
    EXPECT_TRUE(md.contains(QStringLiteral("the open-thing text")));
}

// ----------------------------------------------------------------- INV-10 --
TEST(RoadmapDroppedStatus, Inv10DroppedFirstBulletDetectsAsAntsV1) {
    const QByteArray body = "# Roadmap\n\n- \xF0\x9F\x9A\xAB [DEMO-0001] **Gone.**\n"
                            "- \xF0\x9F\x9A\xAB [DEMO-0002] **Also gone.**\n";
    EXPECT_EQ(RoadmapParse::detectRoadmapFormat(
                  QString::fromUtf8(body).split(QLatin1Char('\n'))),
              QStringLiteral("ants-v1"));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("ROADMAP.md")), body));
    EXPECT_EQ(ProjectLayoutEngine::scanLayout(dir.path()).roadmap.format,
              QStringLiteral("ants-v1"));
}

// ----------------------------------------------------------------- INV-11 --
TEST(RoadmapDroppedStatus, Inv11DroppedIsNeverOpen) {
    EXPECT_FALSE(RoadmapRender::isOpen(QStringLiteral("dropped")));
    Fx fx;
    ASSERT_TRUE(fx.setUp(antsFixture(), true));
    RemoteControl rc(nullptr);
    // DEMO-0008 has no Layman line; an open status would trip the gate.
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(
        flipReq(fx.root, QStringLiteral("DEMO-0008"), QStringLiteral("dropped"))).object();
    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
}
