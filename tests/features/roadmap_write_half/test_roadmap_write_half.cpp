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
#include <utility>

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
// ANTS-4434 widened `withGateOffender` from a bool to a COUNT. One offender and
// two are different states, not degrees of the same one: at one, a single-item
// repair is the last outstanding and commits; at two, each single repair is
// refused by the other and rolls back, so the project cannot be repaired one
// item at a time at all. A bool cannot express the second.
QByteArray fixture(int gateOffenders = 0) {
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
    if (gateOffenders >= 1)
        b += "- \xF0\x9F\x93\x8B [DEMO-0005] **An open item with no Layman line.**\n"
             "  Kind: chore.\n"
             "  Source: seed.\n"
             "\n";
    if (gateOffenders >= 2)
        b += "- \xF0\x9F\x93\x8B [DEMO-0006] **A second open item with no Layman line.**\n"
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

// ANTS-4434 — the gate reads the `layman` COLUMN, so the column is what a
// repair has to be asserted against. Asserting on the rendered file instead
// would pass on a body that merely contains the text without the trailer having
// been parsed into the column the gate consults.
QString laymanOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return QString();
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return QString();
    const auto item = store->readItem(*pk, &err);
    return item ? item->layman : QString();
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

// A failed render leaves the store as it was. The reachable failure is the
// render's own gate, and since ANTS-4628 that means the write's OWN offender:
// the flip below targets DEMO-0005, which is open and carries no Layman line,
// so touching it puts it in the gate's scope and the write is refused. The
// store must not keep the status change made behind that refusal.
//
// It targeted the BLAMELESS DEMO-0007 until 2026-08-24, when the gate was
// per project and an unrelated offender was enough to refuse any write. That
// is no longer a failure at all — Ants4628UntouchedDebtDoesNotBlockAWrite
// below now asserts it SUCCEEDS — so INV-1 needed a failure it can still
// reach. Flipping to in-progress and not to shipped is load-bearing: shipped
// is closed, closed items are never gated, and the write would pass for the
// wrong reason.
TEST(RoadmapWriteHalf, Inv1RenderFailureRollsBack) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/1), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("flip");
        req[QStringLiteral("to_status")]  = QStringLiteral("in-progress");
        req[QStringLiteral("id")]         = QStringLiteral("DEMO-0005");
        resp = rc.cmdRoadmapLogFlipForTest(req).object();
    }

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_gate_unmet"));
    // The refusal names the offender, which since ANTS-4628 is an item this
    // write touched — so it is always one the caller can act on.
    const QJsonArray gateFailures =
        resp.value(QStringLiteral("gate_failures")).toArray();
    ASSERT_EQ(gateFailures.size(), 1);
    EXPECT_EQ(gateFailures.at(0).toString(), QStringLiteral("DEMO-0005"));

    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0005"), projectId),
              QStringLiteral("planned"))
        << "a store write must not survive the render that refused it";
}

// ------------------------------------------------------------- ANTS-4593 -----

// A dry run runs `mutate` inside the transaction and rolls it back (ANTS-4548),
// so the caller's candidate row IS in the store when the gate evaluates. Its id
// was then reported in `gate_failures` beside genuine offenders — and it exists
// nowhere afterwards, so a caller greps for it, finds nothing, and cannot tell
// a bad input from a diverged store.
//
// The two need different actions: fix the call, versus go and repair the
// roadmap. They are now different keys.
TEST(RoadmapWriteHalf, Ants4593PreviewOwnIdIsNotAGateFailure) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    // No pre-existing offenders: the ONLY thing the gate can catch is the row
    // this call proposes, which is the case that must not read as project debt.
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/0), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("append");
        req[QStringLiteral("section")]    = QStringLiteral("work");
        req[QStringLiteral("status")]     = QStringLiteral("planned");
        req[QStringLiteral("kind")]       = QStringLiteral("chore");
        req[QStringLiteral("source")]     = QStringLiteral("test");
        req[QStringLiteral("headline")]   =
            QStringLiteral("A bullet with no Layman line.");
        req[QStringLiteral("dry_run")]    = true;      // the whole point
        resp = rc.cmdRoadmapLogAppendForTest(req).object();
    }

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_gate_unmet"));

    // The preview's own candidate is reported apart from real offenders...
    const QJsonArray own =
        resp.value(QStringLiteral("request_gate_failures")).toArray();
    ASSERT_EQ(own.size(), 1)
        << "the row this call proposes must be named, and named separately";

    // ...and gate_failures, which elsewhere names ids that really exist, is
    // empty. This is the assertion that fails against the pre-fix tree.
    EXPECT_TRUE(resp.value(QStringLiteral("gate_failures")).toArray().isEmpty())
        << "a rolled-back candidate id must never appear in gate_failures";

    // The message must not send the caller looking for that id in the roadmap.
    const QString err = resp.value(QStringLiteral("error")).toString();
    EXPECT_NE(err.indexOf(QStringLiteral("this call would append")), -1)
        << "when the only offender is the proposed row, the refusal is about "
           "the arguments, not about the roadmap: " << err.toStdString();

    // And nothing was written — the id is genuinely rolled back.
    EXPECT_TRUE(statusOf(own.at(0).toString(), projectId).isEmpty())
        << "the candidate row must not survive the preview that named it";
}

// ------------------------------------------------------------- ANTS-4591 -----

// ANTS-4556 gave the FILE-backed bad_section refusal ranked `candidates[]` +
// `sections_total`. This arm — the one every migrated project takes — kept a
// bare message, so the discoverability feature reached the path almost nobody
// runs. Same shape ANTS-4634 had to repair for `would_be_id`.
TEST(RoadmapWriteHalf, Ants4591StoreSectionRefusalRanksCandidates) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/0), &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("append");
        // A near-miss of the fixture's real "work" heading, so the ranker has
        // something to find. A slug sharing no characters would prove nothing.
        req[QStringLiteral("section")]    = QStringLiteral("wrok");
        req[QStringLiteral("status")]     = QStringLiteral("planned");
        req[QStringLiteral("kind")]       = QStringLiteral("chore");
        req[QStringLiteral("source")]     = QStringLiteral("test");
        req[QStringLiteral("layman")]     = QStringLiteral("A summary.");
        req[QStringLiteral("headline")]   = QStringLiteral("A bullet.");
        resp = rc.cmdRoadmapLogAppendForTest(req).object();
    }

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("section_not_found"));
    // sections_total is what tells a caller whether an empty candidates list
    // means "no near miss" or "no sections at all".
    EXPECT_GT(resp.value(QStringLiteral("sections_total")).toInt(), 0);
    const QJsonArray cands =
        resp.value(QStringLiteral("candidates")).toArray();
    ASSERT_FALSE(cands.isEmpty())
        << "a near-miss slug must be offered, or the caller's only route is a "
           "full section_index round-trip";
    QStringList got;
    for (const QJsonValue &v : cands) got << v.toString();
    EXPECT_TRUE(got.contains(QStringLiteral("work")))
        << "ranker did not surface the obvious near miss: "
        << got.join(QStringLiteral(",")).toStdString();
}

// ------------------------------------------------------------- ANTS-4628 -----

// The inversion of ANTS-4434's deadlock, and the case that proves the gate's
// scope really is the items a write touched.
//
// Under whole-project scoping a single repair could not land while any other
// offender remained: the fix applied inside the transaction, the gate then
// refused on the rest, and the rollback took the good repair with it. Measured
// on the live store 2026-08-23 — MAME_Curator had exactly two offenders and
// annotating either came back render_gate_unmet naming only the OTHER.
//
// With the scope narrowed, repairing one offender touches only that item, so
// the gate sees no in-scope offender and it commits. Both halves are asserted:
// the repair lands, AND the other offender is still uncured afterwards — which
// is what proves it was ignored rather than silently fixed.
TEST(RoadmapWriteHalf, Ants4628SingleItemRepairNowCommits) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/2), &projectId);
    ASSERT_FALSE(root.isEmpty());

    // A note whose FIRST line declares the trailer key is what writes the
    // column; mid-line it would be prose, and the column would stay empty.
    const QString repair = QStringLiteral("Layman: A plain-language summary.");

    const auto repairOne = [&](const QString &id) {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("annotate");
        req[QStringLiteral("id")]         = id;
        req[QStringLiteral("note")]       = repair;
        return rc.cmdRoadmapLogFlipForTest(req).object();
    };

    // Repair ONE of the two. It touches only DEMO-0005, so only DEMO-0005 is
    // judged, and the repair inside the transaction leaves nothing in scope to
    // refuse. Before ANTS-4628 this was refused by DEMO-0006 and rolled back.
    {
        const QJsonObject resp = repairOne(QStringLiteral("DEMO-0005"));
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
            << "a single repair must commit now that the gate judges only what "
               "the write touched; got code "
            << resp.value(QStringLiteral("code")).toString().toStdString();
        EXPECT_FALSE(laymanOf(QStringLiteral("DEMO-0005"), projectId).isEmpty())
            << "the repair must have survived the commit";
    }

    // The OTHER offender is untouched and still uncured. This is the half that
    // makes the case meaningful: it proves DEMO-0006 was left out of the gate's
    // scope, rather than the render having quietly cured or dropped it.
    EXPECT_TRUE(laymanOf(QStringLiteral("DEMO-0006"), projectId).isEmpty())
        << "an untouched offender must be neither judged nor modified";

    // And the batch route still works — it is no longer the only escape, but it
    // is still how N items are repaired in one call.
    {
        const QJsonObject resp = repairOne(QStringLiteral("DEMO-0006"));
        EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool());
        EXPECT_FALSE(laymanOf(QStringLiteral("DEMO-0006"), projectId).isEmpty());
    }

    // Both together via flip_batch: still passes, still commits.
    QJsonObject batch;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("flip_batch");
        // The status they already hold — flip_batch is the only op taking N
        // locators with per-locator notes, so a same-status flip is how a batch
        // annotate is expressed. Flipping them to something else would clear
        // `isOpen` and pass the gate for the wrong reason.
        req[QStringLiteral("to_status")] = QStringLiteral("planned");
        QJsonArray locators;
        for (const QString &id : {QStringLiteral("DEMO-0005"),
                                  QStringLiteral("DEMO-0006")}) {
            QJsonObject loc;
            loc[QStringLiteral("id")]   = id;
            loc[QStringLiteral("note")] = repair;
            locators.append(loc);
        }
        req[QStringLiteral("locators")] = locators;
        batch = rc.cmdRoadmapLogFlipBatchForTest(req).object();
    }

    EXPECT_TRUE(batch.value(QStringLiteral("ok")).toBool())
        << "one batch repairing every offender must pass the gate; got code "
        << batch.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_TRUE(batch.value(QStringLiteral("skipped")).toArray().isEmpty());

    for (const QString &id : {QStringLiteral("DEMO-0005"),
                              QStringLiteral("DEMO-0006")}) {
        EXPECT_FALSE(laymanOf(id, projectId).isEmpty())
            << id.toStdString() << " must carry a layman value after the batch";
        EXPECT_EQ(statusOf(id, projectId), QStringLiteral("planned"))
            << "the same-status flip must leave the item open, so the gate was "
               "satisfied by the repair rather than by the item closing";
    }
}

// ANTS-4628 — the case the whole change exists for: an untouched offender does
// not block an unrelated write. This is the exact scenario
// `Inv1RenderFailureRollsBack` asserted the OPPOSITE of until 2026-08-24, so
// the pair of them is the before/after of the decision.
TEST(RoadmapWriteHalf, Ants4628UntouchedDebtDoesNotBlockAWrite) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/1), &projectId);
    ASSERT_FALSE(root.isEmpty());

    // DEMO-0007 is blameless — open, and carrying its Layman line. DEMO-0005 is
    // an offender and is NOT touched by this flip.
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

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "an untouched offender must not refuse this write; got code "
        << resp.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("shipped"));

    // The offender is still an offender. The write was not allowed through by
    // the debt having been cleared behind the caller's back.
    EXPECT_TRUE(laymanOf(QStringLiteral("DEMO-0005"), projectId).isEmpty());
}

// ANTS-4628 — and the other side of it, which is what stops the narrowing from
// being an exemption: a write that touches an offender is still refused, and
// the refusal names that item and no other.
TEST(RoadmapWriteHalf, Ants4628WriteIsStillRefusedByItsOwnOffender) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/2), &projectId);
    ASSERT_FALSE(root.isEmpty());

    // A note that does NOT declare the trailer: it edits the body and leaves
    // the layman column empty, so the item stays an offender and, being
    // touched, is in scope.
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("op")]         = QStringLiteral("annotate");
        req[QStringLiteral("id")]         = QStringLiteral("DEMO-0005");
        req[QStringLiteral("note")]       = QStringLiteral("Just some prose.");
        resp = rc.cmdRoadmapLogFlipForTest(req).object();
    }

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_gate_unmet"));

    const QJsonArray failures =
        resp.value(QStringLiteral("gate_failures")).toArray();
    ASSERT_EQ(failures.size(), 1)
        << "only the touched offender is judged — DEMO-0006 is untouched and "
           "must not appear";
    EXPECT_EQ(failures.at(0).toString(), QStringLiteral("DEMO-0005"));
}

// ANTS-4628 — op:render on a project carrying legacy debt. Its mutation writes
// no item, so its scope is empty and it publishes. This is the deadlock
// dissolving: before the change this refused render_gate_unmet, and since the
// ids live only in the store, no locator could reach them to repair them.
TEST(RoadmapWriteHalf, Ants4628RenderPublishesPastLegacyDebt) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root =
        seedMigrated(guard, tmp, fixture(/*gateOffenders=*/2), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    const QByteArray before = readAll(roadmap);
    ASSERT_FALSE(before.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("render");
    const QJsonObject env = rc.cmdRoadmapLogRenderForTest(r).object();

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << "op:render must publish past untouched gate debt; code="
        << env.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_FALSE(env.value(QStringLiteral("files_written")).toArray().isEmpty());
    EXPECT_NE(readAll(roadmap), before);

    // Both offenders are still uncured. The publish did not invent summaries to
    // get past its own gate, which would be the wrong way to make this pass.
    EXPECT_TRUE(laymanOf(QStringLiteral("DEMO-0005"), projectId).isEmpty());
    EXPECT_TRUE(laymanOf(QStringLiteral("DEMO-0006"), projectId).isEmpty());
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
    // ANTS-4634 — the key moved, the invariant did not: a preview still tells
    // you the id a real call would allocate, now under `would_be_id`. This
    // assertion read `id` and was the reason the store path kept emitting it
    // after ANTS-4508 forbade exactly that — the rule reached the markdown
    // branch, and a test here held the store branch at the old behaviour.
    EXPECT_EQ(resp.value(QStringLiteral("would_be_id")).toString(),
              QStringLiteral("DEMO-0008"))
        << "a preview still reports the id a real call would allocate";
    EXPECT_FALSE(resp.contains(QStringLiteral("id")))
        << "and not under `id`, which a caller reading one field takes for a "
           "reservation (ANTS-4508)";

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


// ------------------------------------------------------------- ANTS-4463 ----

// A dry run emits NO past-tense field.
//
// Reported by LocalWebServerManager, who verified three ways that nothing was
// written — git status clean, a grep for the note text returning 0, the bullet
// still planned — while the envelope said `files_written:["…/ROADMAP.md"]` and
// `note_appended:true`. The only signal that nothing happened was the `dry_run`
// flag beside them, so a caller branching on `files_written` (the obvious field
// to check) read a preview as a completed write. On a phase close that is a
// bullet silently left open while the session reports it shipped.
//
// The fix keeps both properties they asked for: the misleading NAME is gone, so
// it cannot be misread, and the useful VALUE survives under `would_write` —
// matching what changelog_log's dry_run already documents for `bytes`.
TEST(RoadmapWriteHalf, Ants4463DryRunEmitsNoPastTenseFields) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);

    QJsonObject req = appendReq(root, QStringLiteral("A dry-run only bullet."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject dry = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << "precondition: the dry run itself must succeed; got code="
        << dry.value(QStringLiteral("code")).toString().toStdString()
        << " error=" << dry.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_TRUE(dry.value(QStringLiteral("dry_run")).toBool());

    EXPECT_FALSE(dry.contains(QStringLiteral("files_written")))
        << "ANTS-4463: `files_written` is an assertion that the write "
           "happened. On a preview it is absent, not merely accompanied by a "
           "flag — an absent field cannot be misread";
    EXPECT_FALSE(dry.contains(QStringLiteral("note_appended")))
        << "ANTS-4463: the same defect in a boolean — the past tense IS the "
           "claim";

    // The information is not lost, only honestly named.
    ASSERT_TRUE(dry.contains(QStringLiteral("would_write")))
        << "ANTS-4463: dropping the misleading name must not cost the caller "
           "the list of files the write would touch";
    EXPECT_FALSE(dry.value(QStringLiteral("would_write")).toArray().isEmpty());

    // And the real write is unchanged — the past-tense field is correct there
    // precisely because the action did happen.
    QJsonObject realReq = appendReq(root, QStringLiteral("A real bullet."));
    const QJsonObject real = rc.cmdRoadmapLogAppendForTest(realReq).object();
    ASSERT_TRUE(real.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(real.contains(QStringLiteral("dry_run")));
    EXPECT_TRUE(real.contains(QStringLiteral("files_written")))
        << "the real path keeps the past-tense name — this fix narrows the "
           "claim to the path where it is true, it does not remove it";
    EXPECT_FALSE(real.contains(QStringLiteral("would_write")));
}


// ------------------------------------------------- ANTS-4462 / ANTS-4465 ----

// The publish reports the hand-edits it overwrote.
//
// On a migrated project every roadmap_log op re-renders the WHOLE file from the
// store, so any byte that arrived from outside the store is silently reverted.
// Two reporters hit the same mechanism from opposite ends: LocalWebServerManager
// edited the file preamble by hand and watched the next `flip` put it back
// (ANTS-4465), and finbreak hand-flipped two bullets to shipped with ~40 lines
// of resolution prose and found the store still serving the pre-edit text —
// where the next write would have published the stale version over them
// (ANTS-4462's data-loss half). Both landed under an `ok:true` envelope with
// nothing in it saying anything had been lost, and `items_rendered` matching
// the file's bullet count reads as reassurance while counting items, not
// content.
//
// One measurement answers both: render the store BEFORE the mutation and
// compare that against the file on disk. What differs is exactly what did not
// come from the store.
TEST(RoadmapWriteHalf, Ants4462ReportsDiscardedExternalEdits) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    RemoteControl rc(nullptr);

    // 1. The FIRST write after a migration, which is not the quiet case and
    //    must not be dressed up as one. A just-migrated file has never been
    //    rendered, so it still carries the author's bytes wherever the store
    //    keeps a canonical form instead — this fixture's table separator is
    //    `|---|---|` and the render emits `| --- | --- |` (ANTS-3832 stores a
    //    table as canonical JSON rather than replaying it). That is text the
    //    publish really is about to overwrite, so reporting it is the honest
    //    answer, not a false positive. It fires once per project and then goes
    //    quiet, because the write that reports it also canonicalises the file.
    const QJsonObject first =
        rc.cmdRoadmapLogAppendForTest(appendReq(root, QStringLiteral("A first bullet."))).object();
    ASSERT_TRUE(first.value(QStringLiteral("ok")).toBool())
        << "precondition: code=" << first.value(QStringLiteral("code")).toString().toStdString()
        << " error=" << first.value(QStringLiteral("error")).toString().toStdString();
    ASSERT_TRUE(first.contains(QStringLiteral("discarded_external_edits")))
        << "ANTS-4462: the field is absent only when nothing measured the file. "
           "A store-backed write measured it, so it must report — silence here "
           "is indistinguishable from the defect";
    EXPECT_TRUE(first.value(QStringLiteral("discarded_external_edits")).toBool())
        << "the migration's own normalisation is a real overwrite and is "
           "reported as one; suppressing it would mean modelling which "
           "differences are cosmetic, which is a judgement this check does not "
           "have and should not invent";
    EXPECT_GE(first.value(QStringLiteral("discarded_edit_lines")).toInt(), 1);

    // 2. The second write, over a file the render itself just wrote. This is
    //    the arm that matters most: a check that cried wolf on every healthy
    //    write would be worse than no check, because it would be switched off.
    const QJsonObject clean =
        rc.cmdRoadmapLogAppendForTest(appendReq(root, QStringLiteral("A clean bullet."))).object();
    ASSERT_TRUE(clean.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(clean.value(QStringLiteral("discarded_external_edits")).toBool())
        << "a write over a file that is already the store's own render "
           "discards nothing";
    EXPECT_FALSE(clean.contains(QStringLiteral("discarded_edit_lines")))
        << "the count rides on the true arm only; a zero on every healthy "
           "write is a field nobody reads";

    // 3. Now the reported defect: edit the file outside the store. The preamble
    //    is ANTS-4465's own case — it is outside every bullet, so no verb can
    //    write it and a hand-edit is the ONLY way to change it.
    QByteArray hand = readAll(roadmap);
    ASSERT_FALSE(hand.isEmpty());
    const QByteArray marker = "> **Current version:** 9.9.9 — set by hand.\n";
    const int cut = hand.indexOf('\n');
    ASSERT_GT(cut, 0);
    hand.insert(cut + 1, marker);
    ASSERT_TRUE(writeFile(roadmap, hand));

    const QJsonObject dirty =
        rc.cmdRoadmapLogAppendForTest(appendReq(root, QStringLiteral("A bullet after the hand-edit."))).object();
    ASSERT_TRUE(dirty.value(QStringLiteral("ok")).toBool())
        << "the report must not become a refusal: one hand-edit anywhere would "
           "then brick every op on the project — the shape render_gate_unmet "
           "HAD when these items were filed (ANTS-4628 has since scoped that "
           "gate; drift is per FILE and has no equivalent scope, so the "
           "argument stands)";
    EXPECT_TRUE(dirty.value(QStringLiteral("discarded_external_edits")).toBool())
        << "ANTS-4465: the hand-written preamble line was overwritten by the "
           "re-render and the envelope said nothing";
    EXPECT_GE(dirty.value(QStringLiteral("discarded_edit_lines")).toInt(), 1)
        << "ANTS-4465: the caller needs the size of what was lost, not just "
           "that something was";

    // The revert itself is the behaviour being reported, not a second bug: the
    // store is primary and the render publishes it. What the fix adds is that
    // the caller is told.
    EXPECT_FALSE(readAll(roadmap).contains(marker))
        << "precondition for the assertions above: the hand-edit really was "
           "discarded, so the report is describing something that happened";

    // 4. And the drift is not sticky — the write that reported it also
    //    republished the file, so the next write starts clean again.
    const QJsonObject after =
        rc.cmdRoadmapLogAppendForTest(appendReq(root, QStringLiteral("A bullet after the revert."))).object();
    ASSERT_TRUE(after.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(after.value(QStringLiteral("discarded_external_edits")).toBool())
        << "a stale true would train callers to ignore the field";
}

// ANTS-4729 — the renderer's own format marker is not a discarded hand-edit.
//
// Reported from Rolodex: on a migrated project whose ROADMAP.md was rendered
// before the format marker existed, the first write reported
// discarded_external_edits:true — the marker comment and its blank line, which
// the RENDER emits and the older file never had. Every content counter in the
// same envelope read zero.
//
// The flag is documented as the one place a silently discarded hand-edit
// surfaces, so a session is told to stop and investigate whenever it fires.
// Here the investigation finds nothing anybody wrote — which is how a
// high-trust flag becomes one that gets skimmed, and how a genuine discard
// later gets waved through.
TEST(RoadmapWriteHalf, Ants4729RenderOnlyMarkerIsNotADiscard) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    RemoteControl rc(nullptr);

    // Canonicalise first: this write publishes the store's render, so the file
    // becomes exactly what the render emits — marker included. Without it the
    // migration's own normalisation would be in the diff too, and the test
    // could not attribute the flag to the marker alone.
    ASSERT_TRUE(rc.cmdRoadmapLogAppendForTest(
                      appendReq(root, QStringLiteral("A canonicalising bullet.")))
                    .object()
                    .value(QStringLiteral("ok"))
                    .toBool());

    const QByteArray rendered = readAll(roadmap);
    ASSERT_TRUE(rendered.contains("ants-roadmap-format"))
        << "precondition: the render emits the format marker, which is the "
           "line this test strips; without it there is no render-only line "
           "and the test would pass vacuously";

    // Strip ONLY the marker line, reproducing a file rendered before it
    // existed. Nothing else changes, so every remaining difference is a line
    // the render holds and the file lacks.
    QByteArrayList lines = rendered.split('\n');
    int removed = 0;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).contains("ants-roadmap-format")) {
            lines.removeAt(i);
            ++removed;
            break;
        }
    }
    ASSERT_EQ(1, removed);
    ASSERT_TRUE(writeFile(roadmap, lines.join('\n')));

    const QJsonObject env =
        rc.cmdRoadmapLogAppendForTest(
              appendReq(root, QStringLiteral("A bullet after the marker was stripped.")))
            .object();
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << "precondition: code="
        << env.value(QStringLiteral("code")).toString().toStdString();

    EXPECT_TRUE(readAll(roadmap).contains("ants-roadmap-format"))
        << "precondition for the assertions below: the render really did "
           "re-add the marker, so there WAS a difference to classify";
    EXPECT_FALSE(env.value(QStringLiteral("discarded_external_edits")).toBool())
        << "ANTS-4729: the marker is a line the RENDER adds and the file "
           "lacked. An addition can never be content the publish discarded, "
           "so flagging it fires this check on the renderer's own output";
    EXPECT_FALSE(env.contains(QStringLiteral("discarded_edit_lines")))
        << "the count rides the true arm only, so a false flag must not "
           "carry one";
}

// ANTS-4730 — check_sync reports sync_checked on BOTH arms.
//
// Reported from perch. The schema says sync_checked:false means nobody looked
// and is NOT a clean bill of health — which instructs a caller to branch on
// that field before trusting the answer. On the in-sync arm the envelope
// carried file_in_sync:true and no sync_checked key at all, so a caller doing
// exactly what the schema said read absent as falsy and concluded nobody
// looked, on the one response that proves somebody did. The key was emitted
// only where it would be false, which defeats the misread it exists to
// prevent — and lands on the careful caller rather than the careless one.
TEST(RoadmapWriteHalf, Ants4730SyncCheckedPresentOnTheHealthyArm) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    // Publish first, so the file IS the store's render and the check takes its
    // HEALTHY arm — the arm the defect was on.
    ASSERT_TRUE(rc.cmdRoadmapLogAppendForTest(
                      appendReq(root, QStringLiteral("A canonicalising bullet.")))
                    .object()
                    .value(QStringLiteral("ok"))
                    .toBool());

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("check_sync")] = true;
    const QJsonObject env = rc.cmdRoadmapQuery(req).object();

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << "precondition: code="
        << env.value(QStringLiteral("code")).toString().toStdString();
    ASSERT_TRUE(env.value(QStringLiteral("file_in_sync")).toBool())
        << "precondition: the write above published the store, so the file is "
           "in sync and this is the healthy arm";
    ASSERT_TRUE(env.contains(QStringLiteral("sync_checked")))
        << "ANTS-4730: absence on the arm that proves the check ran is read "
           "as falsy by a caller following the schema, so it reports that "
           "nobody looked";
    EXPECT_TRUE(env.value(QStringLiteral("sync_checked")).toBool())
        << "the render-and-compare ran, so the field must say so";
}

// ANTS-4614 — op:"render" publishes the store with no semantic change.
//
// roadmap_migrate reports markdown_rewritten:false honestly (ANTS-4482) and
// nothing owned the doing half: the canonical re-render landed only on the next
// semantic write. On LottoTracker that was not cosmetic — the file carried two
// id dialects the store would normalise, so a wanted normalisation sat
// undelivered. The only route was to invent a semantic write purely as a render
// trigger, which pollutes the roadmap with a bullet nobody wanted, and the
// migration stayed unverifiable from the repo side: a clean git status after
// migrating is indistinguishable from the migration never having run.
TEST(RoadmapWriteHalf, Ants4614RenderPublishesWithoutASemanticWrite) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    // Straight after migration the file is still the author's bytes — the
    // migration computed a normalisation and could not land it.
    const QByteArray before = readAll(roadmap);
    ASSERT_FALSE(before.isEmpty());

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("render");
    const QJsonObject env = rc.cmdRoadmapLogRenderForTest(r).object();

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool())
        << "code=" << env.value(QStringLiteral("code")).toString().toStdString()
        << " error=" << env.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(env.value(QStringLiteral("op")).toString(), QStringLiteral("render"));
    EXPECT_FALSE(env.value(QStringLiteral("files_written")).toArray().isEmpty())
        << "ANTS-4614: the publish must name what it wrote";
    EXPECT_GT(env.value(QStringLiteral("items_rendered")).toInt(), 0);
    EXPECT_GT(env.value(QStringLiteral("bytes_written")).toDouble(), 0.0)
        << "ANTS-4614: the caller needs to see the render landed without "
           "re-reading the file";

    // The artefact: the file really changed, and it changed into the store's
    // own render. Without this the op could report success having done nothing,
    // which is the state it was filed to end.
    EXPECT_NE(readAll(roadmap), before)
        << "ANTS-4614: the normalisation must actually land";

    // And no bullet was invented to trigger it. That is the whole point: the
    // workaround this replaces added a roadmap item nobody wanted. The fixture
    // carries two, and after the publish it must still carry exactly two.
    auto store = openStore(RoadmapStore::Access::Interactive);
    ASSERT_NE(store, nullptr);
    QString err;
    const auto items = store->readItems(projectId, &err);
    ASSERT_TRUE(items.has_value()) << err.toStdString();
    EXPECT_EQ(items->size(), 2)
        << "ANTS-4614: render must add no items — it is not a semantic write";
}

// ANTS-4614 — idempotence. The second render publishes the same bytes and
// reports no drift, which is what makes the op safe to reach for: a caller who
// is unsure whether the file is current can just run it.
TEST(RoadmapWriteHalf, Ants4614SecondRenderIsQuiet) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("render");
    ASSERT_TRUE(rc.cmdRoadmapLogRenderForTest(r).object()
                    .value(QStringLiteral("ok")).toBool());
    const QByteArray settled = readAll(roadmap);

    const QJsonObject again = rc.cmdRoadmapLogRenderForTest(r).object();
    ASSERT_TRUE(again.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(again.value(QStringLiteral("discarded_external_edits")).toBool())
        << "ANTS-4614: a render over the store's own output discards nothing";
    EXPECT_EQ(readAll(roadmap), settled)
        << "ANTS-4614: the render is idempotent";
}

// ANTS-4614 — dry_run previews and writes nothing. ANTS-4463's tense rule
// reaches this op like every other: a past-tense name IS the assertion, so a
// preview must not carry one.
TEST(RoadmapWriteHalf, Ants4614DryRunPreviewsAndWritesNothing) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    const QByteArray before = readAll(roadmap);

    RemoteControl rc(nullptr);
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = QStringLiteral("render");
    r[QStringLiteral("dry_run")]    = true;
    const QJsonObject env = rc.cmdRoadmapLogRenderForTest(r).object();

    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(env.value(QStringLiteral("dry_run")).toBool());
    EXPECT_FALSE(env.value(QStringLiteral("would_write")).toArray().isEmpty())
        << "ANTS-4614: a preview must still say what it WOULD write";
    EXPECT_FALSE(env.contains(QStringLiteral("files_written")))
        << "ANTS-4463: no past-tense field on a preview";
    EXPECT_FALSE(env.contains(QStringLiteral("bytes_written")))
        << "ANTS-4463: no bytes were written, so the assertion must be absent";
    EXPECT_EQ(readAll(roadmap), before)
        << "ANTS-4614: a preview must leave the file byte-identical";
}

// ANTS-4615 — one count cannot be acted on. A status flip that changed nothing
// reported discarded_edit_lines:84; of those, the overwhelming majority were 24
// bullets moving from an older bold-id form to the canonical bracketed one, and
// ONE was a sentence that no longer exists anywhere in the file. A single number
// mixing the two trains callers to wave the flag through, which the reporter
// says is what nearly happened.
//
// This does NOT suppress anything — ANTS-4462's case above is explicit that
// deciding which differences are cosmetic is a judgement this check should not
// invent, and `discarded_edit_lines` keeps counting every drifted line in both
// directions. What is added is a BREAKDOWN of the file's own lines plus the
// lost text itself, so the caller can act instead of guessing.
TEST(RoadmapWriteHalf, Ants4615SplitsRestyledFromLostText) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    RemoteControl rc(nullptr);
    // Canonicalise first, so the migration's own normalisation is spent and
    // this case measures only what it plants.
    ASSERT_TRUE(rc.cmdRoadmapLogAppendForTest(
        appendReq(root, QStringLiteral("A settling bullet."))).object()
        .value(QStringLiteral("ok")).toBool());

    QByteArray hand = readAll(roadmap);
    ASSERT_FALSE(hand.isEmpty());

    // (a) RESTYLED — the same bullet in an older id dialect. Every word of it
    //     survives into the render; only the styling differs. This is the class
    //     that made up 24 of the reporter's 84.
    const QByteArray canonical = "- \xF0\x9F\x93\x8B [DEMO-0008] **A settling bullet.**";
    const QByteArray restyled  = "- TODO **DEMO-0008** A settling bullet.";
    ASSERT_TRUE(hand.contains(canonical))
        << "precondition: the render's canonical bullet form was not found";
    hand.replace(canonical, restyled);

    // (b) LOST — a sentence that exists only in the file. This is the one line
    //     out of 84 that actually mattered, and the one a caller must see.
    const QByteArray lost =
        "> A hand-written sentence that exists nowhere in the store.\n";
    const int cut = hand.indexOf('\n');
    ASSERT_GT(cut, 0);
    hand.insert(cut + 1, lost);
    ASSERT_TRUE(writeFile(roadmap, hand));

    const QJsonObject env = rc.cmdRoadmapLogAppendForTest(
        appendReq(root, QStringLiteral("A bullet after the hand-edits."))).object();
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(env.value(QStringLiteral("discarded_external_edits")).toBool())
        << "precondition: both hand-edits should have registered as drift";

    // The total is unchanged in meaning — still every drifted line, both
    // directions. ANTS-4462's contract is not narrowed by this item.
    EXPECT_GE(env.value(QStringLiteral("discarded_edit_lines")).toInt(), 2);

    // The split. `discarded_text_lines` is the number to act on.
    EXPECT_GE(env.value(QStringLiteral("discarded_restyled_lines")).toInt(), 1)
        << "ANTS-4615: the id-dialect line's text survives; it is restyling";
    EXPECT_EQ(env.value(QStringLiteral("discarded_text_lines")).toInt(), 1)
        << "ANTS-4615: exactly one line's text does not survive — if restyling "
           "leaks into this count the field is as unusable as the single total";

    // And the lost text is NAMED. A count alone still leaves the caller
    // grepping for a sentence they have to remember writing, which is how
    // ANTS-4596 was found.
    const QJsonArray text = env.value(QStringLiteral("discarded_text")).toArray();
    ASSERT_EQ(text.size(), 1) << "ANTS-4615: the lost line must be reported";
    EXPECT_TRUE(text.at(0).toString().contains(
        QStringLiteral("exists nowhere in the store")))
        << "got: " << text.at(0).toString().toStdString();
}

// ANTS-4695 — a punctuation-only change to the author's own prose is neither
// a dialect restyle nor lost text, and lumping it into `restyled` is what let
// a render report `discarded_text_lines: 0` while every Layman line in the
// file was about to change. The Layman: parse drops one trailing period by
// design (ANTS-1154 INV-4), so a project whose Layman lines were hand-authored
// with periods hits this on its first render — thirty of thirty, in the report.
TEST(RoadmapWriteHalf, Ants4695CountsRepunctuationApartFromRestyling) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    RemoteControl rc(nullptr);
    // Store a Layman value with NO terminal period. That is the state a
    // MIGRATED project is left in: the markdown parse chops one trailing
    // period on the way into the column (ANTS-1154 INV-4), so an author who
    // wrote "…is normal." has "…is normal" stored. Setting it directly is the
    // same end state without depending on the migration fixture's prose.
    QJsonObject req = appendReq(root, QStringLiteral("A settling bullet."));
    req[QStringLiteral("layman")] = QStringLiteral("A new thing");
    ASSERT_TRUE(rc.cmdRoadmapLogAppendForTest(req).object()
        .value(QStringLiteral("ok")).toBool());

    QByteArray hand = readAll(roadmap);
    ASSERT_FALSE(hand.isEmpty());

    const QByteArray rendered = "**Layman:** A new thing";
    ASSERT_TRUE(hand.contains(rendered))
        << "precondition: the render's Layman line was not found";
    ASSERT_FALSE(hand.contains(rendered + "."))
        << "precondition: the store holds no period, so the render emits none";

    // Now the author's own file, which still ends the sentence properly.
    hand.replace(rendered + "\n", rendered + ".\n");
    ASSERT_TRUE(writeFile(roadmap, hand));

    const QJsonObject env = rc.cmdRoadmapLogAppendForTest(
        appendReq(root, QStringLiteral("A bullet after the hand-edit."))).object();
    ASSERT_TRUE(env.value(QStringLiteral("ok")).toBool());
    ASSERT_TRUE(env.value(QStringLiteral("discarded_external_edits")).toBool())
        << "precondition: the period should have registered as drift";

    EXPECT_GE(env.value(QStringLiteral("discarded_repunctuated_lines")).toInt(), 1)
        << "ANTS-4695: a terminal-punctuation change must be counted, and "
           "counted as its own thing";

    // The two claims either side of it are unchanged, and that is the point:
    // `discarded_text_lines: 0` must keep meaning "your text is untouched".
    EXPECT_EQ(env.value(QStringLiteral("discarded_text_lines")).toInt(), 0)
        << "ANTS-4695: punctuation is not lost prose";
    EXPECT_EQ(env.value(QStringLiteral("discarded_restyled_lines")).toInt(), 0)
        << "ANTS-4695: nor is it a dialect restyle — if it leaks back into "
           "that count the caller is where they started";
}

// ANTS-4615 — the quiet case. A healthy write must not start emitting the new
// fields: a breakdown present on every write is a breakdown nobody reads, which
// is the failure mode the item is about in the first place.
TEST(RoadmapWriteHalf, Ants4615BreakdownRidesOnTheTrueArmOnly) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);
    ASSERT_TRUE(rc.cmdRoadmapLogAppendForTest(
        appendReq(root, QStringLiteral("A settling bullet."))).object()
        .value(QStringLiteral("ok")).toBool());

    const QJsonObject clean = rc.cmdRoadmapLogAppendForTest(
        appendReq(root, QStringLiteral("A clean bullet."))).object();
    ASSERT_TRUE(clean.value(QStringLiteral("ok")).toBool());
    ASSERT_FALSE(clean.value(QStringLiteral("discarded_external_edits")).toBool())
        << "precondition: the second write should be clean";
    EXPECT_FALSE(clean.contains(QStringLiteral("discarded_text_lines")));
    EXPECT_FALSE(clean.contains(QStringLiteral("discarded_restyled_lines")));
    EXPECT_FALSE(clean.contains(QStringLiteral("discarded_text")));
}

// A preview reports what the real call WOULD discard, in the future tense.
//
// ANTS-4463 settled the rule for this envelope: a past-tense name IS the
// assertion, so a dry run must not carry one. The same rule reaches these two
// fields — and a preview is exactly where a caller most wants the warning,
// because it is the call made before deciding whether to write at all.
TEST(RoadmapWriteHalf, Ants4462DryRunUsesTheFutureTense) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QString roadmap = root + QStringLiteral("/ROADMAP.md");

    QByteArray hand = readAll(roadmap);
    ASSERT_FALSE(hand.isEmpty());
    const int cut = hand.indexOf('\n');
    ASSERT_GT(cut, 0);
    hand.insert(cut + 1, "> A line no verb can write.\n");
    ASSERT_TRUE(writeFile(roadmap, hand));

    RemoteControl rc(nullptr);
    QJsonObject req = appendReq(root, QStringLiteral("A previewed bullet."));
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject dry = rc.cmdRoadmapLogAppendForTest(req).object();

    ASSERT_TRUE(dry.value(QStringLiteral("ok")).toBool())
        << "precondition: code=" << dry.value(QStringLiteral("code")).toString().toStdString();
    EXPECT_TRUE(dry.value(QStringLiteral("would_discard_external_edits")).toBool())
        << "ANTS-4462: the preview is where the warning is worth most";
    EXPECT_GE(dry.value(QStringLiteral("would_discard_edit_lines")).toInt(), 1);
    EXPECT_FALSE(dry.contains(QStringLiteral("discarded_external_edits")))
        << "ANTS-4463: a dry run discarded nothing, so it must not say it did";
    EXPECT_FALSE(dry.contains(QStringLiteral("discarded_edit_lines")));

    // And a preview writes nothing, so the hand-edit is still there.
    EXPECT_TRUE(readAll(roadmap).contains("> A line no verb can write."));
}


// ------------------------------------------------------------- ANTS-4475 ----

// roadmap_log accepts changelog_log's spelling of the same act.
//
// The two sibling write verbs both append an entry to a Markdown record and
// name the op differently — `append` here, `add` there — and each already
// takes a batch variant under the OTHER convention's stem (append_batch /
// add_batch), which is what makes reaching for the wrong one natural rather
// than careless. A LocalWebServerManager session wrote both calls in one
// message and was refused on the spelling alone.
//
// Aliased on the ANTS-3698 precedent (roadmap_query takes `filter` for
// `status`). The reporter was explicit that the refusal itself was a GOOD one
// — it named the op, said it was unknown, and enumerated every valid value —
// so this is filed as cheap-to-fix, not as a defect in the error path.
//
// Driven through cmdRoadmapLog, NOT through the *ForTest seam: the alias lives
// in the op dispatch, and the seam enters BELOW it. A first cut of this test
// used the seam, passed against pre-fix source, and proved nothing — the
// dispatch it was meant to exercise was never reached.
TEST(RoadmapWriteHalf, Ants4475OpAliasAcceptsTheSiblingSpelling) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, fixture(), &projectId);
    ASSERT_FALSE(root.isEmpty());

    RemoteControl rc(nullptr);

    // `add_batch` is the half that can be asserted all the way to a write:
    // append_batch is main-window-independent, so it completes under a
    // headless RemoteControl.
    QJsonObject one = appendReq(root, QStringLiteral("Filed under add_batch."));
    one.remove(QStringLiteral("op"));
    one.remove(QStringLiteral("caller_cwd"));
    one.remove(QStringLiteral("section"));
    QJsonArray bullets;
    bullets.append(one);

    QJsonObject batch;
    batch[QStringLiteral("caller_cwd")] = root;
    batch[QStringLiteral("op")]         = QStringLiteral("add_batch");
    batch[QStringLiteral("section")]    = QStringLiteral("work");
    batch[QStringLiteral("bullets")]    = bullets;
    const QJsonObject resp = rc.cmdRoadmapLog(batch).object();

    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << "ANTS-4475: `add_batch` is changelog_log's spelling of this same "
           "act and must not be refused; got code="
        << resp.value(QStringLiteral("code")).toString().toStdString()
        << " error=" << resp.value(QStringLiteral("error")).toString()
                            .toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("applied_count")).toInt(), 1);

    // The alias is a way IN, never a second name to learn: the envelope keeps
    // reporting the canonical op, so nothing downstream learns the alias.
    EXPECT_EQ(resp.value(QStringLiteral("op")).toString(),
              QStringLiteral("append_batch"))
        << "ANTS-4475: echoing the alias back would teach it as a primary "
           "name and split the vocabulary this fix exists to converge";

    // The singular `add` reaches the append path too. It cannot complete under
    // a headless RemoteControl (that path needs a main window), so what is
    // asserted is that it is no longer turned away at the DISPATCH — pre-fix
    // this was bad_op_combo.
    QJsonObject single = appendReq(root, QStringLiteral("Filed under add."));
    single[QStringLiteral("op")] = QStringLiteral("add");
    const QJsonObject singleResp = rc.cmdRoadmapLog(single).object();
    EXPECT_NE(singleResp.value(QStringLiteral("code")).toString(),
              QStringLiteral("bad_op_combo"))
        << "ANTS-4475: `add` must route to the append path rather than being "
           "refused as an unknown op";
}
