// Feature-conformance test for ANTS-5638 — the header id high-water mark, and
// the rebuild that reads it. See tests/features/roadmap_id_high_water_mark/
// spec.md; the rules are roadmap-format.md § 3.5.1's.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmaprender.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <utility>

namespace {

using Pairs = QMap<QString, qint64>;

const char *kNotice =
    "<!-- Generated from the Ants Terminal roadmap store. Edit it with "
    "roadmap_log; hand edits are discarded by the next write. -->";

// A hand-written ants-v1 roadmap: two prefixes, a synthesised id the file
// already carries, and one bullet with no id, which the load synthesises.
const char *kSeed =
    "<!-- ants-roadmap-format: 1 -->\n"
    "\n"
    "# Demo Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0003] **Third.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n"
    "- \xF0\x9F\x93\x8B [OTHER-0002] **Another prefix.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n"
    "- \xF0\x9F\x93\x8B [DEMO-S0004] **Synthesised once already.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n"
    "- \xF0\x9F\x93\x8B **No id yet.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";

// What a store render of another machine's project looks like: the notice,
// then `markLine`. `extra` is appended to the one section.
QByteArray renderedRoadmap(const QString &markLine, const QString &extra = QString()) {
    QString s = QStringLiteral("<!-- ants-roadmap-format: 1 -->\n") +
                QString::fromUtf8(kNotice) + QLatin1Char('\n');
    if (!markLine.isEmpty())
        s += markLine + QLatin1Char('\n');
    // fromUtf8, not QStringLiteral: a `\x` escape in a u"" literal is one
    // UTF-16 code unit, so the emoji's UTF-8 bytes would arrive as four
    // Latin-1 characters and the bullet would not parse.
    s += QString::fromUtf8(
        "\n"
        "# Demo Roadmap\n"
        "\n"
        "## To Do\n"
        "\n"
        "- \xF0\x9F\x93\x8B [DEMO-0003] **Third.**\n"
        "  Layman: A thing.\n"
        "  Kind: chore.\n"
        "  Source: seed.\n");
    s += extra;
    return s.toUtf8();
}

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(body) == body.size();
}

QString readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

QStringList markLines(const QString &text) {
    QStringList out;
    for (const QString &l : text.split(QLatin1Char('\n')))
        if (l.contains(QLatin1String("ants-roadmap-id-high-water")))
            out.append(l);
    return out;
}

// A project root under `tmp`, canonical, with `body` as its roadmap.
QString makeRoot(const QTemporaryDir &tmp, const QByteArray &body) {
    const QString raw = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(raw + QStringLiteral("/ROADMAP.md"), body))
        return QString();
    return QFileInfo(raw).canonicalFilePath();
}

std::unique_ptr<RoadmapStore> openStore(const QString &path) {
    auto store = std::make_unique<RoadmapStore>(
        path, RoadmapStore::kDefaultHistoryCapBytes, RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

RoadmapMigrate::MigrationPlan planRoot(const QString &root) {
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return {};
    }
    return RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                    QStringLiteral("demo"));
}

RoadmapMigrateLoad::Outcome loadPlan(RoadmapStore &store,
                                     const RoadmapMigrate::MigrationPlan &plan,
                                     const QString &root, bool raiseIdMark) {
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-10-11T10:00:00Z");
    opts.projectRoot = root;
    opts.raiseIdMark = raiseIdMark;
    return RoadmapMigrateLoad::load(store, plan, opts);
}

bool renderTo(RoadmapStore &store, qint64 pid, const QString &root) {
    RoadmapRender::Options ropts;
    ropts.liveRoadmapPath = QStringLiteral("ROADMAP.md");
    QString err;
    const auto out = RoadmapRender::render(store, pid, root, ropts, &err);
    if (!out) {
        ADD_FAILURE() << "render: " << err.toStdString();
        return false;
    }
    if (!out->gateFailures.isEmpty()) {
        ADD_FAILURE() << "render gate: "
                      << out->gateFailures.join(QStringLiteral(", ")).toStdString();
        return false;
    }
    return true;
}

// The seeded store INV-2 to INV-4 read: kSeed loaded, then two rows no item
// explains — one the grammar admits and one it refuses, with its counter.
struct Seeded {
    QTemporaryDir tmp;
    QString root;
    std::unique_ptr<RoadmapStore> store;
    qint64 pid = 0;
};

bool seed(Seeded &s) {
    s.root = makeRoot(s.tmp, QByteArray(kSeed));
    if (s.root.isEmpty())
        return false;
    s.store = openStore(QDir(s.tmp.path()).filePath(QStringLiteral("store.sqlite")));
    if (!s.store)
        return false;
    const auto out = loadPlan(*s.store, planRoot(s.root), s.root, false);
    if (!out.ok) {
        ADD_FAILURE() << "load: " << out.error.toStdString();
        return false;
    }
    s.pid = out.projectId;
    QString err;
    const std::pair<const char *, int> rows[] = {{"GONE", 9}, {"PC G", 4}, {"PC G#S", 2}};
    for (const auto &[key, n] : rows) {
        if (!s.store->raiseIdHighWater(s.pid, QString::fromLatin1(key), n, &err)) {
            ADD_FAILURE() << "raiseIdHighWater: " << err.toStdString();
            return false;
        }
    }
    return true;
}

// DEMO-0003 is the item floor (its row is the 0 the load ensures); the file's
// DEMO-S0004 puts the synthesis at S0005; GONE has a row and no item; OTHER has
// an item and no row; `PC G` fails the grammar and gets nothing.
Pairs seededPairs() {
    return {{QStringLiteral("DEMO"), 3},
            {QStringLiteral("DEMO#S"), 5},
            {QStringLiteral("GONE"), 9},
            {QStringLiteral("OTHER"), 2}};
}
QString seededLine() {
    return QStringLiteral(
        "<!-- ants-roadmap-id-high-water: DEMO=3 DEMO#S=5 GONE=9 OTHER=2 -->");
}

}  // namespace

// ----------------------------------------------------------------- INV-1 ----

TEST(RoadmapIdHighWaterMark, Inv1PrefixGrammar) {
    using RoadmapParse::isIdPrefix;
    EXPECT_TRUE(isIdPrefix(QStringLiteral("ANTS")));
    EXPECT_TRUE(isIdPrefix(QStringLiteral("3D_E")));
    EXPECT_TRUE(isIdPrefix(QStringLiteral("My-Prj")));
    EXPECT_FALSE(isIdPrefix(QStringLiteral("2026"))) << "no letter";
    EXPECT_FALSE(isIdPrefix(QStringLiteral("_X")));
    EXPECT_FALSE(isIdPrefix(QStringLiteral("PC G")));
    EXPECT_FALSE(isIdPrefix(QStringLiteral("ANTS#S")));
    EXPECT_FALSE(isIdPrefix(QStringLiteral("ABCDEFGHIJKLMNOPQ"))) << "17 characters";
    EXPECT_FALSE(isIdPrefix(QString()));
}

TEST(RoadmapIdHighWaterMark, Inv1FormatSortsByKeyInByteOrder) {
    const Pairs pairs{{QStringLiteral("ANTS#S"), 12},
                      {QStringLiteral("ANTS"), 5644},
                      {QStringLiteral("3D_E"), 7}};
    EXPECT_EQ(RoadmapParse::formatIdHighWaterMark(pairs),
              QStringLiteral("<!-- ants-roadmap-id-high-water: "
                             "3D_E=7 ANTS=5644 ANTS#S=12 -->"));
    EXPECT_EQ(RoadmapParse::formatIdHighWaterMark({}), QString())
        << "no pair, no line";
}

TEST(RoadmapIdHighWaterMark, Inv1ReadsTheLineAfterTheNotice) {
    const Pairs pairs{{QStringLiteral("ANTS"), 5644}, {QStringLiteral("ANTS#S"), 3}};
    const QStringList lines{QStringLiteral("<!-- ants-roadmap-format: 1 -->"),
                            QString::fromUtf8(kNotice),
                            RoadmapParse::formatIdHighWaterMark(pairs),
                            QString(), QStringLiteral("# Title")};
    const auto mark = RoadmapParse::findIdHighWaterMark(lines);
    EXPECT_TRUE(mark.present);
    EXPECT_TRUE(mark.valid);
    EXPECT_EQ(mark.line, 3);
    EXPECT_EQ(mark.pairs, pairs);

    // One line further down is not "directly after the notice".
    QStringList gap = lines;
    gap.insert(2, QString());
    EXPECT_FALSE(RoadmapParse::findIdHighWaterMark(gap).present);

    // No notice, no mark, whatever the next line says.
    QStringList noNotice = lines;
    noNotice.removeAt(1);
    EXPECT_FALSE(RoadmapParse::findIdHighWaterMark(noNotice).present);
}

TEST(RoadmapIdHighWaterMark, Inv1UnparseableMarkIsPresentButNotValid) {
    const QString tag = QStringLiteral("<!-- ants-roadmap-id-high-water:");
    for (const QString &rest : {QStringLiteral(" ANTS=12x -->"),
                                QStringLiteral(" ANTS=12"),
                                QStringLiteral(" _X=3 -->"),
                                QStringLiteral(" ANTS=1  OTHER=2 -->"),
                                QStringLiteral(" ANTS -->"),
                                QStringLiteral(" ANTS=1 ANTS=2 -->"),
                                QStringLiteral(" -->")}) {
        const QStringList lines{QStringLiteral("<!-- ants-roadmap-format: 1 -->"),
                                QString::fromUtf8(kNotice), tag + rest};
        const auto mark = RoadmapParse::findIdHighWaterMark(lines);
        EXPECT_TRUE(mark.present) << rest.toStdString();
        EXPECT_FALSE(mark.valid) << rest.toStdString();
        EXPECT_TRUE(mark.pairs.isEmpty()) << rest.toStdString();
        EXPECT_EQ(mark.line, 3) << rest.toStdString();
    }
}

// ----------------------------------------------------------------- INV-2 ----

TEST(RoadmapIdHighWaterMark, Inv2PairsCoverRowsAndStoredIds) {
    Seeded s;
    ASSERT_TRUE(seed(s));
    QString err;
    const auto pairs = s.store->idHighWaterMark(s.pid, &err);
    ASSERT_TRUE(pairs.has_value()) << err.toStdString();
    EXPECT_EQ(*pairs, seededPairs());
}

// ----------------------------------------------------------------- INV-3 ----

TEST(RoadmapIdHighWaterMark, Inv3RenderWritesTheMarkUnderTheNotice) {
    Seeded s;
    ASSERT_TRUE(seed(s));
    ASSERT_TRUE(renderTo(*s.store, s.pid, s.root));
    const QString first = readAll(s.root + QStringLiteral("/ROADMAP.md"));
    const QStringList lines = first.split(QLatin1Char('\n'));
    ASSERT_GE(lines.size(), 3);
    EXPECT_TRUE(lines.at(1).contains(QLatin1String("Generated from the Ants Terminal")))
        << first.left(400).toStdString();
    EXPECT_EQ(lines.at(2), seededLine()) << first.left(400).toStdString();

    ASSERT_TRUE(renderTo(*s.store, s.pid, s.root));
    EXPECT_EQ(readAll(s.root + QStringLiteral("/ROADMAP.md")), first)
        << "two renders of one store must write the same bytes";
}

// ----------------------------------------------------------------- INV-4 ----

TEST(RoadmapIdHighWaterMark, Inv4ReMigrationKeepsOneMark) {
    Seeded s;
    ASSERT_TRUE(seed(s));
    ASSERT_TRUE(renderTo(*s.store, s.pid, s.root));

    const auto plan = planRoot(s.root);
    ASSERT_TRUE(plan.idMark.has_value());
    EXPECT_TRUE(plan.idMark->valid);
    EXPECT_EQ(plan.idMark->pairs, seededPairs());
    for (const auto &sec : plan.sections)
        EXPECT_FALSE(sec.intro.contains(QLatin1String("ants-roadmap-id-high-water")))
            << "section '" << sec.slug.toStdString() << "' kept the mark in its intro";

    const auto out = loadPlan(*s.store, plan, s.root, false);
    ASSERT_TRUE(out.ok) << out.error.toStdString();
    ASSERT_TRUE(renderTo(*s.store, s.pid, s.root));
    EXPECT_EQ(markLines(readAll(s.root + QStringLiteral("/ROADMAP.md"))),
              QStringList{seededLine()});
}

// ----------------------------------------------------------------- INV-5 ----

namespace {
const char *kIdLess =
    "- \xF0\x9F\x93\x8B **No id yet.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";
const char *kMark40 = "<!-- ants-roadmap-id-high-water: DEMO=40 DEMO#S=9 -->";
}  // namespace

TEST(RoadmapIdHighWaterMark, Inv5LoadRaisesTheMarkBeforeAllocating) {
    QTemporaryDir tmp;
    const QString root = makeRoot(
        tmp, renderedRoadmap(QString::fromLatin1(kMark40), QString::fromUtf8(kIdLess)));
    ASSERT_FALSE(root.isEmpty());
    auto store = openStore(QDir(tmp.path()).filePath(QStringLiteral("store.sqlite")));
    ASSERT_TRUE(store);

    const auto out = loadPlan(*store, planRoot(root), root, true);
    ASSERT_TRUE(out.ok) << out.error.toStdString();
    ASSERT_EQ(out.idsAllocated, 1);
    EXPECT_EQ(store->idHighWater(out.projectId, QStringLiteral("DEMO")).value_or(-1), 40);
    EXPECT_EQ(store->maxSynthesisedId(out.projectId, QStringLiteral("DEMO")).value_or(-1), 10)
        << "the synthesised id must floor on the mark's DEMO#S=9";
}

TEST(RoadmapIdHighWaterMark, Inv5WithoutTheOptionTheRowsAreUntouched) {
    QTemporaryDir tmp;
    const QString root = makeRoot(
        tmp, renderedRoadmap(QString::fromLatin1(kMark40), QString::fromUtf8(kIdLess)));
    ASSERT_FALSE(root.isEmpty());
    auto store = openStore(QDir(tmp.path()).filePath(QStringLiteral("store.sqlite")));
    ASSERT_TRUE(store);

    const auto out = loadPlan(*store, planRoot(root), root, false);
    ASSERT_TRUE(out.ok) << out.error.toStdString();
    EXPECT_EQ(store->idHighWater(out.projectId, QStringLiteral("DEMO")).value_or(-1), 0);
    EXPECT_EQ(store->maxSynthesisedId(out.projectId, QStringLiteral("DEMO")).value_or(-1), 1);
}

TEST(RoadmapIdHighWaterMark, Inv5RefusedLoadLeavesNoRow) {
    QTemporaryDir tmp;
    const QString root = makeRoot(tmp, renderedRoadmap(QString::fromLatin1(kMark40)));
    ASSERT_FALSE(root.isEmpty());
    auto store = openStore(QDir(tmp.path()).filePath(QStringLiteral("store.sqlite")));
    ASSERT_TRUE(store);

    auto plan = planRoot(root);
    ASSERT_TRUE(plan.idMark.has_value());
    ASSERT_FALSE(plan.items.isEmpty());
    plan.items[0].sectionSlug = QStringLiteral("no-such-section");   // fails after the raise
    const auto out = loadPlan(*store, plan, root, true);
    ASSERT_FALSE(out.ok);
    QString err;
    EXPECT_FALSE(store->projectIdForRoot(root, &err).has_value())
        << "a refused load committed its project row, and with it the mark's rows";
}

// ----------------------------------------------------------------- INV-6 ----

namespace {

// XDG_DATA_HOME moves the store into the fixture. TMPDIR moves the temp dir
// AWAY from it: the rebuild, like roadmap_migrate, never registers a root
// under the temp dir (ANTS-4600), and QTemporaryDir lives there.
QString rebuildRoot(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                    const QByteArray &body) {
    guard.setEnv("XDG_DATA_HOME", QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString otherTmp = QDir(tmp.path()).filePath(QStringLiteral("othertmp"));
    QDir().mkpath(otherTmp);
    guard.setEnv("TMPDIR", otherTmp.toUtf8());
    return makeRoot(tmp, body);
}

QJsonObject appendReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("status")]     = QStringLiteral("planned");
    o[QStringLiteral("headline")]   = QStringLiteral("The first append on this machine.");
    o[QStringLiteral("layman")]     = QStringLiteral("A plain-language line.");
    o[QStringLiteral("kind")]       = QStringLiteral("chore");
    o[QStringLiteral("source")]     = QStringLiteral("ants-5638-test");
    o[QStringLiteral("id_prefix")]  = QStringLiteral("DEMO");
    return o;
}

QJsonObject appendBatchReq(const QString &root) {
    QJsonObject b;
    b[QStringLiteral("status")]   = QStringLiteral("planned");
    b[QStringLiteral("headline")] = QStringLiteral("The first batch on this machine.");
    b[QStringLiteral("layman")]   = QStringLiteral("A plain-language line.");
    b[QStringLiteral("kind")]     = QStringLiteral("chore");
    b[QStringLiteral("source")]   = QStringLiteral("ants-5638-test");
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("id_prefix")]  = QStringLiteral("DEMO");
    o[QStringLiteral("bullets")]    = QJsonArray{b};
    return o;
}

bool storeHasRow(const QString &root) {
    RoadmapStore store(RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Interactive);
    QString err;
    if (!store.open(&err))
        return false;
    return store.projectIdForRoot(root, &err).has_value();
}

std::string dump(const QJsonObject &o) { return QJsonDocument(o).toJson().toStdString(); }

}  // namespace

TEST(RoadmapIdHighWaterMark, Inv6AppendRebuildsAndAllocatesAboveTheMark) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    const QString root = rebuildRoot(guard, tmp, renderedRoadmap(QString::fromLatin1(kMark40)));
    ASSERT_FALSE(root.isEmpty());
    // No storeHasRow() here: opening the store creates its file, and a fresh
    // machine has none. The batch and broken-mark cases rely on the same.
    ASSERT_FALSE(QFileInfo::exists(RoadmapStore::defaultPath()));

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(), QStringLiteral("DEMO-0041"))
        << "the file's own floor is DEMO-0003; the mark's is 40" << dump(resp);
    const QJsonObject rebuilt = resp.value(QStringLiteral("store_rebuilt")).toObject();
    EXPECT_EQ(rebuilt.value(QStringLiteral("id_mark")).toString(), QStringLiteral("applied"))
        << dump(resp);
    EXPECT_TRUE(storeHasRow(root));

    // The project now has a row, so the next write is an ordinary one.
    QJsonObject second = appendReq(root);
    second[QStringLiteral("headline")] = QStringLiteral("The second append.");
    const QJsonObject resp2 = rc.cmdRoadmapLogAppendForTest(second).object();
    ASSERT_TRUE(resp2.value(QStringLiteral("ok")).toBool()) << dump(resp2);
    EXPECT_FALSE(resp2.contains(QStringLiteral("store_rebuilt"))) << dump(resp2);
    EXPECT_EQ(resp2.value(QStringLiteral("id")).toString(), QStringLiteral("DEMO-0042"));
}

TEST(RoadmapIdHighWaterMark, Inv6AppendBatchRebuildsToo) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    const QString root = rebuildRoot(guard, tmp, renderedRoadmap(QString::fromLatin1(kMark40)));
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendBatchForTest(appendBatchReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    const QJsonArray ids = resp.value(QStringLiteral("ids")).toArray();
    ASSERT_EQ(ids.size(), 1) << dump(resp);
    EXPECT_EQ(ids.at(0).toString(), QStringLiteral("DEMO-0041")) << dump(resp);
    EXPECT_EQ(resp.value(QStringLiteral("store_rebuilt")).toObject()
                  .value(QStringLiteral("id_mark")).toString(),
              QStringLiteral("applied")) << dump(resp);
}

TEST(RoadmapIdHighWaterMark, Inv6UnparseableMarkIsIgnoredAndSaidSo) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    const QString root = rebuildRoot(
        guard, tmp,
        renderedRoadmap(QStringLiteral("<!-- ants-roadmap-id-high-water: DEMO=4x0 -->")));
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(), QStringLiteral("DEMO-0004"))
        << "an ignored mark floors on the files' ids alone" << dump(resp);
    EXPECT_EQ(resp.value(QStringLiteral("store_rebuilt")).toObject()
                  .value(QStringLiteral("id_mark")).toString(),
              QStringLiteral("ignored_unparseable")) << dump(resp);
}

TEST(RoadmapIdHighWaterMark, Inv6DryRunRebuildsNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    const QString root = rebuildRoot(guard, tmp, renderedRoadmap(QString::fromLatin1(kMark40)));
    ASSERT_FALSE(root.isEmpty());

    QJsonObject req = appendReq(root);
    req[QStringLiteral("dry_run")] = true;
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();
    EXPECT_FALSE(resp.contains(QStringLiteral("store_rebuilt"))) << dump(resp);
    EXPECT_FALSE(storeHasRow(root)) << "a dry run registered the project";
}

TEST(RoadmapIdHighWaterMark, Inv6FileWithoutTheNoticeIsNotRebuilt) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    const QString root = rebuildRoot(guard, tmp, QByteArray(kSeed));
    ASSERT_FALSE(root.isEmpty());
    // A hand-written roadmap with no store row allocates from the counter,
    // and refuses without one.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/.roadmap-counter"), QByteArray("4\n")));

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool()) << dump(resp);
    EXPECT_FALSE(resp.contains(QStringLiteral("store_rebuilt"))) << dump(resp);
    EXPECT_FALSE(storeHasRow(root)) << "a hand-written roadmap was registered";
}
