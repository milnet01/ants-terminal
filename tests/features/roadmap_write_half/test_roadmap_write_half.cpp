// Feature-conformance test for ANTS-3809 — the write half. Contract:
// tests/features/roadmap_write_half/spec.md
//
// Behavioural, through the roadmap_log verbs themselves: every case migrates a
// small markdown fixture into a store at RoadmapStore::defaultPath() (redirected
// into the case's sandbox), drives a `*ForTest` entry point, and then re-opens
// the store to assert what actually landed. Migrating rather than hand-building
// matters — a hand-built store can hold rows the loader never writes, and an
// invariant asserted against one is asserted against a state the product cannot
// reach.

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

// NEVER default-construct RoadmapStore: defaultPath() resolves the developer's
// REAL store under XDG_DATA_HOME. Every case redirects XDG_DATA_HOME first, and
// `Access` is the THIRD parameter, after the history cap.
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

// ~1.2 KiB of intro prose, so the file clears kRoadmapMinParseableSize (1024 B)
// — below it the flip paths will not trust an ants-v1 walk and refuse
// unrecognised_format before any locator is tried.
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

// The highest-id fixture the cases share: one open item at DEMO-0007 (so the
// corpus floor has something to prove), one shipped item, and a Bundles section
// carrying a GFM table for INV-8.
//
// `withGateOffender` adds a THIRD item that trips the render's INV-5 gate — a
// public OPEN item with no `Layman:`. It has to be a different item from the one
// INV-1 then flips: the gate reads the state the write produced, so flipping the
// offender itself to `shipped` would clear the very condition under test.
QByteArray fixture(bool withGateOffender = false) {
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
        "  Layman: A thing.\n"
        "  Kind: implement.\n"
        "  Source: seed.\n"
        "\n"
        "- \xE2\x9C\x85 [DEMO-0003] **A shipped item.**\n"
        "  Layman: Another thing.\n"
        "  Kind: fix.\n"
        "  Source: seed.\n"
        "\n";
    if (withGateOffender)
        b += "- \xF0\x9F\x93\x8B [DEMO-0005] **An open item with no Layman line.**\n"
             "  Kind: chore.\n"
             "  Source: seed.\n"
             "\n";
    b +=
         "## Bundles\n"
         "\n"
         "| Bundle | Status |\n"
         "|---|---|\n"
         "| one | open |\n";
    return b;
}

// Redirect XDG_DATA_HOME into the sandbox, write the fixture, and migrate it —
// findRoadmaps → planFrom → load, the migration as a consumer runs it. Bulk,
// because RoadmapMigrateLoad::load() refuses an Interactive connection
// (ANTS-3765 INV-12); closed before the verb under test runs.
QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     const QByteArray &markdown, qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), markdown))
        return QString();
    // The store keys a project on its CANONICAL root (ANTS-3756 INV-8), and
    // /tmp is a symlink on some hosts.
    const QString root = QFileInfo(rawRoot).canonicalFilePath();

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return QString();
    }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-08-05T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return QString();
    }
    *projectId = out.projectId;
    return root;
}

QJsonObject appendReq(const QString &root, const QString &headline) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("append");
    req[QStringLiteral("section")]    = QStringLiteral("work");
    req[QStringLiteral("status")]     = QStringLiteral("planned");
    req[QStringLiteral("headline")]   = headline;
    req[QStringLiteral("kind")]       = QStringLiteral("implement");
    req[QStringLiteral("source")]     = QStringLiteral("test");
    req[QStringLiteral("layman")]     = QStringLiteral("A new thing.");
    return req;
}

QString statusOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return QString();
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return QString();
    const auto item = store->readItem(*pk, &err);
    return item ? item->status : QString();
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

// A failed render leaves the store as it was. The reachable failure is the
// render's own gate — it is per PROJECT, so a blameless item's flip is refused
// by an unrelated offender, and must not have persisted behind that refusal.
TEST(RoadmapWriteHalf, Inv1RenderFailureRollsBack) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*withGateOffender=*/true), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("flip");
        req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
        req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
        resp = rc.cmdRoadmapLogFlipForTest(req).object();
    }

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_gate_unmet"));
    // The refusal names the offenders — the gate is per PROJECT, so the
    // caller's own item may be blameless, and naming them is the only way the
    // refusal is actionable.
    const QJsonArray gateFailures =
        resp.value(QStringLiteral("gate_failures")).toArray();
    ASSERT_EQ(gateFailures.size(), 1);
    EXPECT_EQ(gateFailures.at(0).toString(), QStringLiteral("DEMO-0005"));

    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("planned"))
        << "a store write must not survive the render that refused it";
}

// ---------------------------------------------------------------- INV-2 -----

// The render is the only writer: append puts the item in the STORE and the file
// is the render's output, not a splice. The envelope's declared field
// difference (ANTS-3793 INV-2) is the observable half.
TEST(RoadmapWriteHalf, Inv2RenderIsTheOnlyWriter) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogAppendForTest(
                     appendReq(root, QStringLiteral("A stored item.")))
                   .object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_FALSE(resp.value(QStringLiteral("files_written")).toArray().isEmpty());
    EXPECT_GT(resp.value(QStringLiteral("items_rendered")).toInt(), 0);
    EXPECT_FALSE(resp.contains(QStringLiteral("line")))
        << "a store has no lines";
    EXPECT_FALSE(resp.contains(QStringLiteral("bytes_written")));

    const QString newId = resp.value(QStringLiteral("id")).toString();
    ASSERT_FALSE(newId.isEmpty());
    {
        auto store = openStore(RoadmapStore::Access::Interactive);
        ASSERT_NE(store, nullptr);
        QString err;
        EXPECT_TRUE(store->findItem(projectId, newId, &err).has_value())
            << "the item must be in the store, not only in the file";
    }
    EXPECT_TRUE(readAll(root + QStringLiteral("/ROADMAP.md"))
                    .contains(newId.toUtf8()))
        << "the render must have published the new item";
}

// ---------------------------------------------------------------- INV-3 -----

// Allocation floors to the committed corpus and records itself in the store;
// .roadmap-counter is neither read nor written.
TEST(RoadmapWriteHalf, Inv3Allocation) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);

    const QJsonObject first =
        rc.cmdRoadmapLogAppendForTest(appendReq(root, QStringLiteral("First.")))
            .object();
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool())
        << first.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(first.value(QStringLiteral("id")).toString(),
              QStringLiteral("DEMO-0008"))
        << "allocation must floor to the corpus high-water (DEMO-0007), not "
           "restart at 0001";

    const QJsonObject second =
        rc.cmdRoadmapLogAppendForTest(appendReq(root, QStringLiteral("Second.")))
            .object();
    ASSERT_TRUE(second.value(QStringLiteral("ok")).toBool())
        << second.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(second.value(QStringLiteral("id")).toString(),
              QStringLiteral("DEMO-0009"));

    QJsonObject hinted = appendReq(root, QStringLiteral("Hinted."));
    hinted[QStringLiteral("id_hint")] = 5;
    const QJsonObject taken = rc.cmdRoadmapLogAppendForTest(hinted).object();
    EXPECT_FALSE(taken.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(taken.value(QStringLiteral("code")).toString(),
              QStringLiteral("id_taken"));

    EXPECT_FALSE(QFile::exists(root + QStringLiteral("/.roadmap-counter")))
        << "the store path must not create the counter file";
}

// ---------------------------------------------------------------- INV-4 -----

// A body write re-derives the trailer columns the request did not supply. This
// is what makes the render's Layman: gate remediable — the line is body text in
// markdown and a column in the store.
TEST(RoadmapWriteHalf, Inv4BodyDerivesColumns) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("annotate");
        req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
        // Lanes:, because the fixture body carries no Lanes: line — every
        // matcher takes its FIRST match, so a key the fixture already has
        // would test the parser's ordering rather than the re-derivation.
        req[QStringLiteral("note")] = QStringLiteral("Lanes: vt, core.");
        const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(req).object();
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("error")).toString().toStdString();
    }

    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QString err;
    const auto pk = store->findItem(projectId, QStringLiteral("DEMO-0007"), &err);
    ASSERT_TRUE(pk.has_value());
    const auto item = store->readItem(*pk, &err);
    ASSERT_TRUE(item.has_value()) << err.toStdString();

    ASSERT_EQ(item->lanes.size(), 2)
        << "the note's Lanes: line must reach the COLUMN, not only the body";
    EXPECT_EQ(item->lanes.at(0), QStringLiteral("vt"));
    EXPECT_EQ(item->lanes.at(1), QStringLiteral("core"));
    // A key neither body yields is untouched — it came from a request argument
    // or the migration, and this op has no opinion about it.
    EXPECT_TRUE(item->evidence.isEmpty());
    EXPECT_EQ(item->kind, QStringLiteral("implement"));
}

// ---------------------------------------------------------------- INV-5 -----

// A column write the body would out-vote is refused before anything is written.
TEST(RoadmapWriteHalf, Inv5BodyShadowed) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req = appendReq(root, QStringLiteral("A shadowed item."));
        req[QStringLiteral("kind")] = QStringLiteral("fix");
        // A stale trailer line in the body — anchored, and the likeliest
        // shadowing shape there is.
        req[QStringLiteral("body")] = QStringLiteral("Kind: refactor.");
        resp = rc.cmdRoadmapLogAppendForTest(req).object();
    }

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("body_shadowed"));
    const QString message = resp.value(QStringLiteral("error")).toString();
    EXPECT_TRUE(message.contains(QStringLiteral("refactor"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QStringLiteral("stale trailer")))
        << "an anchored match's remedy is to delete or correct that line: "
        << message.toStdString();

    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QString err;
    EXPECT_FALSE(store->findItem(projectId, QStringLiteral("DEMO-0008"), &err)
                     .has_value())
        << "a refused append must write nothing";
}

// ---------------------------------------------------------------- INV-6 -----

// line_range cannot be served by the store, and is refused PER LOCATOR so a
// mixed batch still applies its others.
TEST(RoadmapWriteHalf, Inv6LineRangeRefused) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject byRange;
        byRange[QStringLiteral("line_range")] = QJsonArray{ 1, 9999 };
        QJsonObject byId;
        byId[QStringLiteral("id")] = QStringLiteral("DEMO-0007");

        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
        // in-progress, not shipped: the fixture's OTHER item is already
        // shipped, so a range that silently matched everything would be
        // invisible against that target and visible against this one.
        req[QStringLiteral("to_status")]  = QStringLiteral("in-progress");
        req[QStringLiteral("locators")]   = QJsonArray{ byRange, byId };
        resp = rc.cmdRoadmapLogFlipBatchForTest(req).object();
    }

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    const QJsonArray skipped = resp.value(QStringLiteral("skipped")).toArray();
    ASSERT_EQ(skipped.size(), 1);
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("code")).toString(),
              QStringLiteral("locator_unsupported"));
    EXPECT_EQ(skipped.at(0).toObject().value(QStringLiteral("locator_index")).toInt(), 0);
    EXPECT_EQ(resp.value(QStringLiteral("flipped_count")).toInt(), 1)
        << "the id locator in the same batch must still apply";

    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("in-progress"));
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0003"), projectId),
              QStringLiteral("shipped"))
        << "the refused range must not have flipped the whole roadmap";
}

// ---------------------------------------------------------------- INV-7 -----

// dry_run commits nothing on the store path either — the sequence stops after
// the dry render and rolls back.
TEST(RoadmapWriteHalf, Inv7DryRunCommitsNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QByteArray before = readAll(root + QStringLiteral("/ROADMAP.md"));
    ASSERT_FALSE(before.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req = appendReq(root, QStringLiteral("A previewed item."));
        req[QStringLiteral("dry_run")] = true;
        resp = rc.cmdRoadmapLogAppendForTest(req).object();
    }

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("dry_run")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
              QStringLiteral("DEMO-0008"))
        << "a preview still reports the id a real call would allocate";

    {
        auto store = openStore(RoadmapStore::Access::Interactive);
        ASSERT_NE(store, nullptr);
        QString err;
        EXPECT_FALSE(
            store->findItem(projectId, QStringLiteral("DEMO-0008"), &err).has_value())
            << "dry_run must commit nothing to the store";
    }
    EXPECT_EQ(readAll(root + QStringLiteral("/ROADMAP.md")), before)
        << "dry_run must leave ROADMAP.md byte-identical";
}

// ---------------------------------------------------------------- INV-8 -----

// bundle_row is a read-modify-write of ONE kind='table' element's canonical
// JSON payload — a store assertion, not a rendered-line one.
TEST(RoadmapWriteHalf, Inv8BundleRow) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("bundle_row");
        req[QStringLiteral("section")]    = QStringLiteral("bundles");
        req[QStringLiteral("cells")] =
            QJsonArray{ QStringLiteral("two"), QStringLiteral("done") };
        resp = rc.cmdRoadmapLogBundleRowForTest(req).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("code")).toString().toStdString() << ": "
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_FALSE(resp.value(QStringLiteral("created_table")).toBool())
        << "the migrated table must be read-modify-written, not re-created";

    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QString err;
    const auto sectionId =
        store->findSection(projectId, QStringLiteral("bundles"), &err);
    ASSERT_TRUE(sectionId.has_value()) << err.toStdString();
    const auto elements = store->listElements(*sectionId, &err);
    ASSERT_TRUE(elements.has_value()) << err.toStdString();

    int tables = 0;
    QJsonObject payload;
    for (const RoadmapStore::ElementRow &e : *elements) {
        if (e.kind != QLatin1String("table")) continue;
        ++tables;
        payload = QJsonDocument::fromJson(
                      e.payload.value_or(QString()).toUtf8()).object();
    }
    ASSERT_EQ(tables, 1) << "the op must not insert a second table element";
    const QJsonArray rows = payload.value(QStringLiteral("rows")).toArray();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(1).toArray().at(0).toString(), QStringLiteral("two"));
    EXPECT_EQ(rows.at(1).toArray().at(1).toString(), QStringLiteral("done"));
    EXPECT_EQ(payload.value(QStringLiteral("header")).toArray().size(), 2);
}

// --------------------------------------------------------------- ANTS-3838 --

// `provenance.id` records who SUPPLIED the id, and the two append branches did
// not supply it the same way. roadmap-data-model.md § 7.7 reserves
// `store-generated` for § 4.1's `write (store-populated)` fields, and § 4.1
// marks `id` exactly that — so a counter/high-water allocation is the store's
// value and only an `id_strategy:"stable_prefix"` id is the author's.
//
// Both legs are asserted in one case on purpose: a single-branch assertion
// would pass against a writer that hardcodes either value, which is the defect
// this locks. `id_origin` is checked alongside to pin that the two fields are
// NOT the same question — it stays `synthesised` however the id arrived.
TEST(RoadmapWriteHalf, Ants3838ProvenanceIdPerBranch) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    const auto provenanceIdOf = [&](const QString &id) {
        auto store = openStore(RoadmapStore::Access::Interactive);
        if (!store) return QString();
        QString err;
        const auto pk = store->findItem(projectId, id, &err);
        if (!pk) return QString();
        const auto item = store->readItem(*pk, &err);
        if (!item) return QString();
        return item->provenance.value(QStringLiteral("id")).toString();
    };
    const auto idOriginOf = [&](const QString &id) {
        auto store = openStore(RoadmapStore::Access::Interactive);
        if (!store) return QString();
        QString err;
        const auto pk = store->findItem(projectId, id, &err);
        if (!pk) return QString();
        const auto item = store->readItem(*pk, &err);
        return item ? item->idOrigin : QString();
    };

    // Leg 1 — the store allocated the id. Nobody asserted it.
    QString allocatedId;
    {
        RemoteControl rc(nullptr);
        const QJsonObject resp =
            rc.cmdRoadmapLogAppendForTest(
                  appendReq(root, QStringLiteral("Allocated id.")))
                .object();
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("error")).toString().toStdString();
        allocatedId = resp.value(QStringLiteral("id")).toString();
    }
    ASSERT_FALSE(allocatedId.isEmpty());
    EXPECT_EQ(provenanceIdOf(allocatedId), QStringLiteral("store-generated"))
        << "an id the store allocated was not supplied by the author — "
           "roadmap-data-model.md § 7.7 over § 4.1's `write "
           "(store-populated)` marking";

    // Leg 2 — the caller pinned the id. That one really is asserted.
    {
        RemoteControl rc(nullptr);
        QJsonObject req = appendReq(root, QStringLiteral("Pinned id."));
        req[QStringLiteral("id_strategy")] = QStringLiteral("stable_prefix");
        req[QStringLiteral("stable_id")]   = QStringLiteral("Demo-SP1");
        const QJsonObject resp =
            rc.cmdRoadmapLogAppendForTest(req).object();
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("error")).toString().toStdString();
        EXPECT_EQ(resp.value(QStringLiteral("id")).toString(),
                  QStringLiteral("Demo-SP1"));
    }
    EXPECT_EQ(provenanceIdOf(QStringLiteral("Demo-SP1")),
              QStringLiteral("asserted"))
        << "a caller-pinned stable_id WAS supplied by the author";

    // Leg 3 — `append_batch` is a second, separately-written store path with
    // the same choice to make. Covering only `append` would let the batch path
    // revert unnoticed, which is how the two diverged in the first place.
    QString batchId;
    {
        RemoteControl rc(nullptr);
        QJsonObject bullet;
        bullet[QStringLiteral("status")]   = QStringLiteral("planned");
        bullet[QStringLiteral("headline")] = QStringLiteral("Batched item.");
        bullet[QStringLiteral("kind")]     = QStringLiteral("implement");
        bullet[QStringLiteral("source")]   = QStringLiteral("test");
        bullet[QStringLiteral("layman")]   = QStringLiteral("A batched thing.");

        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("append_batch");
        req[QStringLiteral("section")]    = QStringLiteral("work");
        req[QStringLiteral("bullets")]    = QJsonArray{ bullet };
        const QJsonObject resp =
            rc.cmdRoadmapLogAppendBatchForTest(req).object();
        ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << resp.value(QStringLiteral("error")).toString().toStdString();
        // The store path's batch envelope carries a flat `ids` array (and
        // `applied_count`), not the markdown path's per-bullet objects.
        EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 1);
        const QJsonArray ids = resp.value(QStringLiteral("ids")).toArray();
        ASSERT_EQ(ids.size(), 1);
        batchId = ids.at(0).toString();
    }
    ASSERT_FALSE(batchId.isEmpty());
    EXPECT_EQ(provenanceIdOf(batchId), QStringLiteral("store-generated"))
        << "append_batch allocates ids the same way append does, so it must "
           "record them the same way";

    // The two fields answer different questions, so they must not move
    // together: `id_origin` is `synthesised` on all branches (ANTS-3809's
    // already-settled ruling), whatever `provenance.id` says.
    EXPECT_EQ(idOriginOf(allocatedId), QStringLiteral("synthesised"));
    EXPECT_EQ(idOriginOf(QStringLiteral("Demo-SP1")),
              QStringLiteral("synthesised"));
    EXPECT_EQ(idOriginOf(batchId), QStringLiteral("synthesised"));
}
