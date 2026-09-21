// Feature-conformance test for ANTS-4500 — a synthesised id gets its own
// namespace (`<prefix>-S<NNNN>`) and its own counter (`<prefix>#S`).
// See tests/features/roadmap_synth_id/spec.md.
//
// Migration invents an id for a bullet that carries none. Before this change it
// took the next value in the project's own counter space, so the invented id
// was indistinguishable from a real one and spent the space real items allocate
// from.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapindex.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapparse.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace {

// ------------------------------------------------------------- fixtures ----

// An ants-v1 roadmap, so migratedProject() resolves and roadmap_log takes the
// STORE path — which is what INV-7 and INV-9 assert about.
QByteArray roadmapWith(const QByteArray &bullets) {
    return QByteArray(
               "<!-- ants-roadmap-format: 1 -->\n"
               "\n"
               "# Demo Roadmap\n"
               "\n"
               "## To Do\n"
               "\n")
           + bullets;
}

// A bullet carrying a declared id. The Layman line is required, not decoration:
// the render's INV-5 gate refuses a project whose open items lack one.
QByteArray declared(const char *id, const char *headline) {
    return QByteArray("- \xF0\x9F\x93\x8B [") + id + "] **" + headline
           + "**\n  Layman: A plain-language line.\n  Kind: chore.\n"
             "  Source: ants-4500-test.\n";
}

// Every headline here carries a SPACE, deliberately. A single-word bold run
// (`- \U0001F4CB **One.**`) parses as the legacy bold-ID form, so the word
// becomes the id and the Layman trailer becomes the headline — the bullet is
// then id-BEARING and nothing is synthesised at all.
QByteArray idless(const char *headline) {
    return QByteArray("- \xF0\x9F\x93\x8B **") + headline
           + "**\n  Layman: A plain-language line.\n  Kind: chore.\n"
             "  Source: ants-4500-test.\n";
}

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. Every case here redirects that first.
std::unique_ptr<RoadmapStore> openStore() {
    auto store = std::make_unique<RoadmapStore>(RoadmapStore::defaultPath(),
                                                RoadmapStore::kDefaultHistoryCapBytes,
                                                RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return nullptr;
    }
    return store;
}

// Writes the fixture under `leaf` and returns the CANONICAL root: the store
// keys a project on it (ANTS-3756 INV-8) and /tmp is a symlink on some hosts.
QString seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
             const QByteArray &bullets, const char *leaf = "proj") {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString raw = QDir(tmp.path()).filePath(QString::fromLatin1(leaf));
    if (!writeFile(raw + QStringLiteral("/ROADMAP.md"), roadmapWith(bullets)))
        return QString();
    return QFileInfo(raw).canonicalFilePath();
}

// Rewrites an already-seeded root's roadmap, for the re-migration legs.
bool rewrite(const QString &root, const QByteArray &bullets) {
    return writeFile(root + QStringLiteral("/ROADMAP.md"), roadmapWith(bullets));
}

// ---------------------------------------------------------------- driving --

struct MigrateResult {
    bool    ok = false;
    qint64  allocated = 0;
    int     orphaned = 0;
    QString error;
};

MigrateResult migrate(RoadmapStore &store, const QString &root,
                      const char *stamp = "2026-09-21T10:00:00Z") {
    MigrateResult r;
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        r.error = QStringLiteral("findRoadmaps: ") + err;
        return r;
    }
    // Name and slug from the root's own leaf: `export_slug` is UNIQUE across
    // the whole store, so a hardcoded one collides the moment a case seeds a
    // second project.
    const QString leaf = QFileInfo(root).fileName();
    const auto plan = RoadmapMigrate::planFrom(*disc, leaf, leaf);
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QString::fromLatin1(stamp);
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    r.ok        = out.ok;
    r.allocated = out.idsAllocated;
    r.orphaned  = out.itemsOrphaned;
    r.error     = out.error;
    return r;
}

// One migration against its own store handle, which is the ordinary shape: a
// re-migration in a later leg opens a fresh one.
MigrateResult migrateOnce(const QString &root,
                          const char *stamp = "2026-09-21T10:00:00Z") {
    auto store = openStore();
    if (!store)
        return MigrateResult{};
    return migrate(*store, root, stamp);
}

qint64 projectIdOf(RoadmapStore &store, const QString &root) {
    QString err;
    const auto row = store.readProjectByRoot(root, &err);
    if (!row) {
        ADD_FAILURE() << "readProjectByRoot(" << root.toStdString()
                      << "): " << err.toStdString();
        return 0;
    }
    return row->projectId;
}

// headline -> stored id, for every item in the project. Read through
// readItem() rather than listItems(): ItemRef carries only the case-FOLDED id,
// and `-S` is an assertion about case.
QHash<QString, QString> storedIds(RoadmapStore &store, qint64 projectId) {
    QHash<QString, QString> byHeadline;
    QString err;
    const auto items = store.listItems(projectId, &err);
    if (!items) {
        ADD_FAILURE() << "listItems: " << err.toStdString();
        return byHeadline;
    }
    for (const RoadmapStore::ItemRef &r : *items) {
        const auto row = store.readItem(r.itemPk, &err);
        if (!row) {
            ADD_FAILURE() << "readItem: " << err.toStdString();
            continue;
        }
        byHeadline.insert(row->headline, row->id);
    }
    return byHeadline;
}

// Deletes one id_prefix row, simulating a store that lost its counter — the
// state a restore from an older backup leaves behind. Raw SQL because no store
// surface removes a counter, and none should.
bool dropCounterRow(const QString &prefixKey) {
    bool ok = false;
    const QString conn = QStringLiteral("ants4500_probe");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(RoadmapStore::defaultPath());
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("DELETE FROM id_prefix WHERE prefix = ?"));
            q.addBindValue(prefixKey);
            ok = q.exec();
            db.close();
        } else {
            ADD_FAILURE() << "probe open: " << db.lastError().text().toStdString();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

QJsonObject appendReq(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    o[QStringLiteral("section")]    = QStringLiteral("to-do");
    o[QStringLiteral("status")]     = QStringLiteral("planned");
    o[QStringLiteral("headline")]   = QStringLiteral("An appended bullet.");
    o[QStringLiteral("layman")]     = QStringLiteral("A plain-language line.");
    o[QStringLiteral("kind")]       = QStringLiteral("chore");
    o[QStringLiteral("source")]     = QStringLiteral("ants-4500-test");
    return o;
}

const QRegularExpression &synthShape() {
    static const QRegularExpression rx(
        QStringLiteral("^[A-Za-z0-9_-]+-S[0-9]{4,}$"));
    return rx;
}

}  // namespace

// ----------------------------------------------------------------- INV-1 ----

TEST(RoadmapSynthId, rendersSSuffix) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp,
                              declared("DEMO-0001", "A declared bullet.")
                                  + idless("An invented bullet."));
    ASSERT_FALSE(root.isEmpty());

    const MigrateResult m = migrateOnce(root);
    ASSERT_TRUE(m.ok) << m.error.toStdString();
    ASSERT_EQ(m.allocated, 1) << "the fixture's one id-less bullet must be synthesised";

    auto store = openStore();
    ASSERT_TRUE(store);
    const qint64 pid = projectIdOf(*store, root);
    ASSERT_NE(pid, 0);
    const auto ids = storedIds(*store, pid);

    const QString invented = ids.value(QStringLiteral("An invented bullet."));
    EXPECT_TRUE(synthShape().match(invented).hasMatch())
        << "an invented id must carry the -S infix, got: " << invented.toStdString();
    EXPECT_EQ(invented, QStringLiteral("DEMO-S0001"));
    EXPECT_EQ(ids.value(QStringLiteral("A declared bullet.")),
              QStringLiteral("DEMO-0001"))
        << "a declared id is untouched";
}

// ----------------------------------------------------------------- INV-2 ----
//
// The real prefix's row is ENSURED by migration (INV-9) but never ADVANCED by
// synthesis, so the assertion is that it stays at zero across two synthesising
// runs while the synthesis counter climbs.

TEST(RoadmapSynthId, realCounterUntouched) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp,
                              declared("DEMO-0001", "A declared bullet.")
                                  + idless("First invented."));
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrateOnce(root).ok);

    ASSERT_TRUE(rewrite(root, declared("DEMO-0001", "A declared bullet.")
                                  + idless("First invented.")
                                  + idless("Second invented.")));
    const MigrateResult second = migrateOnce(root, "2026-09-21T11:00:00Z");
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    ASSERT_EQ(second.allocated, 1) << "only the new bullet is synthesised";

    auto store = openStore();
    ASSERT_TRUE(store);
    const qint64 pid = projectIdOf(*store, root);
    ASSERT_NE(pid, 0);

    QString err;
    const auto real = store->idHighWater(pid, QStringLiteral("DEMO"), &err);
    EXPECT_EQ(real.value_or(0), 0)
        << "synthesis advanced the counter roadmap_log allocates from";
    const auto synth = store->idHighWater(pid, QStringLiteral("DEMO#S"), &err);
    ASSERT_TRUE(synth.has_value()) << "the synthesis counter row must exist";
    EXPECT_EQ(*synth, 2) << "two bullets were invented across the two runs";
}

// ----------------------------------------------------------------- INV-3 ----
//
// Three legs, one per term of the floor. Leg 1 is the ordinary case; leg 2
// removes the counter row so only the stored ids can carry the floor; leg 3 is
// a project whose FILE holds an -S id and whose store holds nothing, which only
// the plan-side term can see.

TEST(RoadmapSynthId, synthIdsUnique) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray three = idless("The alpha bullet.") + idless("The bravo bullet.")
                             + idless("The charlie bullet.");
    const QString root = seed(guard, tmp,
                              declared("DEMO-0001", "A declared bullet.") + three);
    ASSERT_FALSE(root.isEmpty());

    ASSERT_TRUE(migrateOnce(root).ok);

    // --- leg 2: the counter row is gone; term 2 reads the ids the items hold.
    ASSERT_TRUE(dropCounterRow(QStringLiteral("DEMO#S")));
    ASSERT_TRUE(rewrite(root, declared("DEMO-0001", "A declared bullet.") + three
                                  + idless("The delta bullet.")
                                  + idless("The echo bullet.")));
    const MigrateResult second = migrateOnce(root, "2026-09-21T11:00:00Z");
    ASSERT_TRUE(second.ok) << second.error.toStdString();

    auto store = openStore();
    ASSERT_TRUE(store);
    const qint64 pid = projectIdOf(*store, root);
    ASSERT_NE(pid, 0);
    const auto ids = storedIds(*store, pid);

    QSet<QString> seen;
    for (auto it = ids.cbegin(); it != ids.cend(); ++it) {
        EXPECT_FALSE(seen.contains(it.value()))
            << "duplicate id after the counter row was lost: "
            << it.value().toStdString();
        seen.insert(it.value());
    }
    EXPECT_EQ(seen.size(), ids.size());
    store.reset();

    // --- leg 3: a fresh project whose SOURCE already carries an -S id and
    // whose store holds nothing for it. Only the plan-side term sees it.
    const QString other = seed(guard, tmp,
                               declared("DEMO-S0007", "An -S id already in the file.")
                                   + idless("Invented beside it."),
                               "other");
    ASSERT_FALSE(other.isEmpty());
    const MigrateResult third = migrateOnce(other, "2026-09-21T12:00:00Z");
    ASSERT_TRUE(third.ok) << third.error.toStdString();

    auto store2 = openStore();
    ASSERT_TRUE(store2);
    const qint64 pid2 = projectIdOf(*store2, other);
    ASSERT_NE(pid2, 0);
    const auto ids2 = storedIds(*store2, pid2);
    const QString invented = ids2.value(QStringLiteral("Invented beside it."));
    EXPECT_NE(invented, QStringLiteral("DEMO-S0007"))
        << "the invented id collided with the one already in the source";
    EXPECT_EQ(invented, QStringLiteral("DEMO-S0008"))
        << "the floor must clear the file's own -S id";
}

// ----------------------------------------------------------------- INV-4 ----

TEST(RoadmapSynthId, synthIdAddressable) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp,
                              declared("DEMO-0001", "A declared bullet.")
                                  + idless("An invented bullet."));
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrateOnce(root).ok);

    RemoteControl rc(nullptr);

    // Publish first. Migration writes the store and leaves ROADMAP.md alone, so
    // the file still carries the bullet id-less — and a write locator resolves
    // the bullet in the FILE before mapping it to its store row
    // (rcdetail::rlStoreItemPk takes a parsed BulletRecord). Rendering is what a
    // real session does next, and it puts the case end to end: the `-S` id
    // reaches the file, and the parser has to read it back as an id.
    QJsonObject render;
    render[QStringLiteral("caller_cwd")] = root;
    render[QStringLiteral("op")]         = QStringLiteral("render");
    const QJsonObject pub = rc.cmdRoadmapLogRenderForTest(render).object();
    ASSERT_TRUE(pub.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(pub).toJson().toStdString();

    QFile f(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray published = f.readAll();
    f.close();
    EXPECT_TRUE(published.contains("[DEMO-S0001]"))
        << "the render did not write the synthesised id into the file";

    QJsonObject read;
    read[QStringLiteral("caller_cwd")] = root;
    read[QStringLiteral("id")]         = QStringLiteral("DEMO-S0001");
    const QJsonObject rResp = rc.cmdRoadmapQueryForTest(read).object();
    EXPECT_TRUE(rResp.value(QStringLiteral("ok")).toBool())
        << "a synthesised id must be fetchable: "
        << QJsonDocument(rResp).toJson().toStdString();

    QJsonObject annotate;
    annotate[QStringLiteral("caller_cwd")] = root;
    annotate[QStringLiteral("op")]         = QStringLiteral("annotate");
    annotate[QStringLiteral("id")]         = QStringLiteral("DEMO-S0001");
    annotate[QStringLiteral("note")]       = QStringLiteral("Checked 2026-09-21.");
    const QJsonObject aResp = rc.cmdRoadmapLogFlipForTest(annotate).object();
    EXPECT_TRUE(aResp.value(QStringLiteral("ok")).toBool())
        << "a synthesised id must be writable: "
        << QJsonDocument(aResp).toJson().toStdString();
}

// ----------------------------------------------------------------- INV-5 ----
//
// A regression guard, not a red-first case: it passes before the change and
// must keep passing. § 3 decision 1 — no retroactive rename.

TEST(RoadmapSynthId, existingIdsUnchanged) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray body = declared("DEMO-0001", "A declared bullet.")
                            + idless("An invented bullet.");
    const QString root = seed(guard, tmp, body);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrateOnce(root).ok);

    QHash<QString, QString> before;
    {
        auto store = openStore();
        ASSERT_TRUE(store);
        const qint64 pid = projectIdOf(*store, root);
        ASSERT_NE(pid, 0);
        before = storedIds(*store, pid);
    }
    ASSERT_FALSE(before.isEmpty());

    const MigrateResult second = migrateOnce(root, "2026-09-21T11:00:00Z");
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_EQ(second.allocated, 0) << "an unchanged source allocates nothing";

    auto store = openStore();
    ASSERT_TRUE(store);
    const qint64 pid = projectIdOf(*store, root);
    ASSERT_NE(pid, 0);
    EXPECT_EQ(storedIds(*store, pid), before)
        << "a re-migration renamed an id that had already settled";
}

// ----------------------------------------------------------------- INV-6 ----

TEST(RoadmapSynthId, reMatchFallsBackToKey) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const char *kHeadline = "A bullet that gains an id.";
    const QString root = seed(guard, tmp,
                              declared("DEMO-0001", "A declared bullet.")
                                  + idless(kHeadline));
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrateOnce(root).ok);

    int countBefore = 0;
    {
        auto store = openStore();
        ASSERT_TRUE(store);
        const qint64 pid = projectIdOf(*store, root);
        ASSERT_NE(pid, 0);
        countBefore = storedIds(*store, pid).size();
    }
    ASSERT_EQ(countBefore, 2);

    // The author files the bullet by hand, keeping its headline.
    ASSERT_TRUE(rewrite(root, declared("DEMO-0001", "A declared bullet.")
                                  + declared("DEMO-0042", kHeadline)));
    const MigrateResult second = migrateOnce(root, "2026-09-21T11:00:00Z");
    ASSERT_TRUE(second.ok) << second.error.toStdString();
    EXPECT_EQ(second.orphaned, 0)
        << "the re-match fell through and orphaned the stored row";

    auto store = openStore();
    ASSERT_TRUE(store);
    const qint64 pid = projectIdOf(*store, root);
    ASSERT_NE(pid, 0);
    const auto ids = storedIds(*store, pid);
    EXPECT_EQ(ids.size(), countBefore) << "the fallback inserted a duplicate item";
    EXPECT_EQ(ids.value(QString::fromLatin1(kHeadline)), QStringLiteral("DEMO-0042"))
        << "the source's id must win, or the next render overwrites the author's";
}

// ----------------------------------------------------------------- INV-7 ----
//
// Guards the `#` exclusion in idPrefixFor(). The synthesis counter is driven
// ABOVE the real one, which is what makes `ORDER BY high_water DESC` return the
// wrong row when the exclusion is absent.

TEST(RoadmapSynthId, synthPrefixNotProjectPrefix) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp,
                              declared("DEMO-0001", "A declared bullet.")
                                  + idless("The first invented bullet.")
                                  + idless("The second invented bullet.")
                                  + idless("The third invented bullet.")
                                  + idless("The fourth invented bullet.")
                                  + idless("The fifth invented bullet."));
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrateOnce(root).ok);

    {
        auto store = openStore();
        ASSERT_TRUE(store);
        const qint64 pid = projectIdOf(*store, root);
        ASSERT_NE(pid, 0);
        QString err;
        const auto synth = store->idHighWater(pid, QStringLiteral("DEMO#S"), &err);
        ASSERT_TRUE(synth.has_value());
        ASSERT_GT(*synth, 1) << "setup: the synthesis counter must exceed the real one";
        EXPECT_EQ(store->idPrefixFor(pid, &err).value_or(QString()),
                  QStringLiteral("DEMO"))
            << "idPrefixFor returned the synthesis counter key";
    }

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const QString id = resp.value(QStringLiteral("id")).toString();
    EXPECT_FALSE(id.contains(QLatin1Char('#')))
        << "allocated through the synthesis counter key: " << id.toStdString();
    EXPECT_FALSE(synthShape().match(id).hasMatch())
        << "an appended id must not carry the -S infix: " << id.toStdString();
    EXPECT_EQ(id, QStringLiteral("DEMO-0002"));
}

// ----------------------------------------------------------------- INV-8 ----

TEST(RoadmapSynthId, synthIdParsesAsId) {
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("DEMO-S0001")))
        << "the canonical-id predicate rejects the synthesised shape";
    EXPECT_TRUE(RoadmapIndex::isCanonicalId(QStringLiteral("3D_E-S0042")))
        << "a digit-led prefix still resolves with the -S infix";
    EXPECT_FALSE(RoadmapIndex::isCanonicalId(QStringLiteral("DEMO-S")))
        << "the -S infix still requires a numeric suffix";

    const auto bullets = RoadmapParse::parseBullets(
        QString::fromUtf8(roadmapWith(declared("DEMO-S0001", "An -S bullet."))));
    bool found = false;
    for (const auto &b : bullets) {
        if (b.headline.contains(QStringLiteral("An -S bullet."))) {
            found = true;
            EXPECT_EQ(b.id, QStringLiteral("DEMO-S0001"))
                << "the parser assigned a content-hash id instead of the authored one";
        }
    }
    EXPECT_TRUE(found) << "the fixture bullet did not parse at all";
}

// ----------------------------------------------------------------- INV-9 ----
//
// Three corpus projects are synthesised in full. Without the real prefix's row
// idPrefixFor() finds only the `#S` key, and the append allocator falls through
// to a directory-leaf guess — two id families in one store.

TEST(RoadmapSynthId, realPrefixRowEnsured) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Every bullet id-less, so the prefix comes from the root's leaf: "proj".
    const QString root = seed(guard, tmp, idless("The only bullet here.")
                                              + idless("Another bullet here."));
    ASSERT_FALSE(root.isEmpty());
    const MigrateResult m = migrateOnce(root);
    ASSERT_TRUE(m.ok) << m.error.toStdString();
    ASSERT_EQ(m.allocated, 2);

    {
        auto store = openStore();
        ASSERT_TRUE(store);
        const qint64 pid = projectIdOf(*store, root);
        ASSERT_NE(pid, 0);
        QString err;
        EXPECT_EQ(store->idPrefixFor(pid, &err).value_or(QString()),
                  QStringLiteral("PROJ"))
            << "a wholly-synthesised project must still resolve its real prefix";
    }

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(appendReq(root)).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(), QStringLiteral("PROJ-0001"))
        << "the append allocator did not use the ensured real prefix";
}
