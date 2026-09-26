// ANTS-5334 — writes on a store-served pass-headings roadmap go through the
// store. Contract: tests/features/roadmap_pass_store_write/spec.md
//
// Behavioural, through the roadmap_log verbs: each case migrates a small
// `#### Pass N.M` roadmap into a store at RoadmapStore::defaultPath() (redirected
// into the case's sandbox, as roadmap_write_half does), drives a `*ForTest`
// entry point, and re-opens the store to assert what landed.

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
#include <QDate>
#include <QTemporaryDir>

#include <memory>

namespace {

const char *kSeed =
    "# Demo — Roadmap\n"
    "\n"
    "## Phase 4\n"
    "\n"
    "#### Pass 43.5 Harden the parser against a truncated frame.\n"
    "- **Status**: done\n"
    "  The frame length is validated before the body is read.\n"
    "\n"
    "#### Pass 43.5.B Follow-up: the same check on the reply path.\n"
    "- **Status**: todo\n"
    "\n"
    "## Phase 5\n"
    "\n"
    "#### Pass 44.1 Retire the legacy transport.\n"
    "- **Status**: in-progress\n"
    "  Blocked on the 43.5 follow-up.\n";

// ANTS-5395 — RetroDB's shape: dated Status lines, `---` between passes.
const char *kDated =
    "# Demo — Roadmap\n"
    "\n"
    "## Phase 59\n"
    "\n"
    "#### Pass 59.41 Close the cache on shutdown.\n"
    "- **Status**: planned (2026-09-01). Lanes: cache, shutdown.\n"
    "The cache is flushed but never closed.\n"
    "\n"
    "---\n"
    "\n"
    "#### Pass 59.42 Log the close.\n"
    "- **Status**: planned (2026-09-01). Lanes: logging.\n"
    "\n"
    "---\n";

// ANTS-5404 — RetroDB's blocks (roadmap.md at 075f494, Passes 59.64, 59.72,
// 59.71), prose shortened, bullets and Status lines exact. A Resolution sits
// above Status; Source and Progress bullets follow it; the project writes
// `shipped (date)`, never `done`.
const char *kRetro =
    "# Demo — Roadmap\n"
    "\n"
    "## Phase 59\n"
    "\n"
    "#### Pass 59.64 MISSING DOCUMENT — the launcher subsystem has no spec (HIGH, M)\n"
    "- **Target**: a new `docs/specs/launcher.md`.\n"
    "- **Resolution** (2026-09-26, docs only, 60bdc74): the spec is accepted.\n"
    "- **Status**: shipped (2026-09-26). Lanes: launch, docs.\n"
    "- **Source**: review-code launch lane 2026-09-01.\n"
    "---\n"
    "\n"
    "#### Pass 59.72 A container-relative media value still resolves against the bundle (MEDIUM, S)\n"
    "- **Target**: `services/media_cleanup.py::_resolve_media_path`.\n"
    "- **Resolution** (2026-09-26, v3.23.13, 9ed9d45): fixed rather than deleted.\n"
    "- **Status**: shipped (2026-09-26). Lanes: media, packaging.\n"
    "- **Source**: in-session 2026-09-02.\n"
    "\n"
    "---\n"
    "\n"
    "#### Pass 59.71 ACTION REQUIRED — rotate the PSN NPSSO token (HIGH, S)\n"
    "- **Target**: the live PSN NPSSO credential.\n"
    "- **Status**: planned (2026-09-02). Lanes: security, packaging.\n"
    "- **Source**: in-session 2026-09-01 (`a5e0939`).\n"
    "- **Progress** (2026-09-26): the user chose to rotate the token.\n"
    "  Waiting on the user.\n"
    "\n"
    "---\n";

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
// REAL store under XDG_DATA_HOME, which every case redirects first.
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

// Write the seed at <tmp>/proj and return its canonical root. With `migrate`,
// also load it into the sandboxed store (Bulk, closed before the verb runs).
QString seed(ants_test::XdgGuard &guard, const QTemporaryDir &tmp, bool migrate,
             qint64 *projectId = nullptr, const char *body = kSeed) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), QByteArray(body)))
        return QString();
    QString root = QFileInfo(rawRoot).canonicalFilePath();
    if (!migrate) return root;

    auto store = openStore(RoadmapStore::Access::Bulk);
    if (!store) return QString();
    QString err;
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << "findRoadmaps: " << err.toStdString(); return QString(); }
    const auto plan =
        RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"), QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-25T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << "migration load: " << out.error.toStdString(); return QString(); }
    if (projectId) *projectId = out.projectId;
    return root;
}

std::optional<RoadmapStore::ItemWrite> itemOf(const QString &id, qint64 projectId) {
    auto store = openStore(RoadmapStore::Access::Interactive);
    if (!store) return std::nullopt;
    QString err;
    const auto pk = store->findItem(projectId, id, &err);
    if (!pk) return std::nullopt;
    return store->readItem(*pk, &err);
}

QJsonObject req(const QString &root, const QString &op) {
    QJsonObject r;
    r[QStringLiteral("caller_cwd")] = root;
    r[QStringLiteral("op")]         = op;
    return r;
}

}  // namespace

// INV-1
TEST(RoadmapPassStoreWrite, Inv1FlipWritesThroughTheStore) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, /*migrate=*/true, &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = req(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-43-5-B");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(), QStringLiteral("pass-headings"));

    const auto item = itemOf(QStringLiteral("PASS-43-5-B"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(item->status, QStringLiteral("shipped"))
        << "the flip did not reach the store";
    const QString file = QString::fromUtf8(readAll(root + QStringLiteral("/ROADMAP.md")));
    EXPECT_TRUE(file.contains(QStringLiteral(
        "#### Pass 43.5.B Follow-up: the same check on the reply path.\n- **Status**: done")))
        << file.toStdString();
}

// INV-2
TEST(RoadmapPassStoreWrite, Inv2DryRunPreviewsWithoutWriting) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QByteArray before = readAll(root + QStringLiteral("/ROADMAP.md"));

    QJsonObject r = req(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-43-5-B");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    r[QStringLiteral("dry_run")]   = true;
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_FALSE(resp.value(QStringLiteral("would_write")).toArray().isEmpty())
        << "a store-path preview names the file it would write";
    EXPECT_EQ(itemOf(QStringLiteral("PASS-43-5-B"), projectId)->status,
              QStringLiteral("planned"));
    EXPECT_EQ(readAll(root + QStringLiteral("/ROADMAP.md")), before);
}

// INV-3
TEST(RoadmapPassStoreWrite, Inv3AnnotateAppendsToTheStoredBody) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, true, &projectId);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = req(root, QStringLiteral("annotate"));
    r[QStringLiteral("id")]   = QStringLiteral("PASS-44-1");
    r[QStringLiteral("note")] = QStringLiteral("Unblocked: the follow-up landed.");
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    const auto item = itemOf(QStringLiteral("PASS-44-1"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->body.contains(QStringLiteral("Unblocked: the follow-up landed.")))
        << item->body.toStdString();
    EXPECT_EQ(item->status, QStringLiteral("in-progress"));
}

// INV-4
TEST(RoadmapPassStoreWrite, Inv4OpsWithNoStoreRouteRefuse) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, true, &projectId);
    ASSERT_FALSE(root.isEmpty());
    const QByteArray before = readAll(root + QStringLiteral("/ROADMAP.md"));
    const QString bodyBefore = itemOf(QStringLiteral("PASS-44-1"), projectId)->body;

    QJsonObject append = req(root, QStringLiteral("append"));
    append[QStringLiteral("section")]  = QStringLiteral("phase-5");
    append[QStringLiteral("status")]   = QStringLiteral("planned");
    append[QStringLiteral("pass")]     = QStringLiteral("44.2");
    append[QStringLiteral("headline")] = QStringLiteral("A new pass.");
    append[QStringLiteral("kind")]     = QStringLiteral("implement");
    append[QStringLiteral("source")]   = QStringLiteral("test");

    QJsonObject appendBatch = req(root, QStringLiteral("append_batch"));
    appendBatch[QStringLiteral("section")] = QStringLiteral("phase-5");
    QJsonObject b;
    b[QStringLiteral("status")]   = QStringLiteral("planned");
    b[QStringLiteral("pass")]     = QStringLiteral("44.2");
    b[QStringLiteral("headline")] = QStringLiteral("A new pass.");
    b[QStringLiteral("kind")]     = QStringLiteral("implement");
    b[QStringLiteral("source")]   = QStringLiteral("test");
    appendBatch[QStringLiteral("bullets")] = QJsonArray{b};

    QJsonObject loc;
    loc[QStringLiteral("id")]   = QStringLiteral("PASS-43-5-B");
    loc[QStringLiteral("note")] = QStringLiteral("A batch note.");
    QJsonObject flipBatch = req(root, QStringLiteral("flip_batch"));
    flipBatch[QStringLiteral("to_status")] = QStringLiteral("shipped");
    flipBatch[QStringLiteral("locators")]  = QJsonArray{loc};
    QJsonObject annotateBatch = req(root, QStringLiteral("annotate_batch"));
    QJsonObject aloc = loc;
    aloc[QStringLiteral("id")] = QStringLiteral("PASS-44-1");
    annotateBatch[QStringLiteral("locators")] = QJsonArray{aloc};

    // ANTS-5396 — amend_body refuses on this format too, through its own
    // message; RetroDB reached it first.
    QJsonObject amend = req(root, QStringLiteral("amend_body"));
    amend[QStringLiteral("id")]       = QStringLiteral("PASS-44-1");
    amend[QStringLiteral("old_text")] = QStringLiteral("x");
    amend[QStringLiteral("new_text")] = QStringLiteral("y");

    RemoteControl rc(nullptr);
    const struct { const char *op; QJsonObject resp; } cases[] = {
        {"amend_body",     rc.cmdRoadmapLogAmendBodyForTest(amend).object()},
        {"append",         rc.cmdRoadmapLogAppendForTest(append).object()},
        {"append_batch",   rc.cmdRoadmapLogAppendBatchForTest(appendBatch).object()},
        {"flip_batch",     rc.cmdRoadmapLogFlipBatchForTest(flipBatch).object()},
        {"annotate_batch", rc.cmdRoadmapLogFlipBatchForTest(annotateBatch).object()},
    };
    for (const auto &c : cases) {
        EXPECT_FALSE(c.resp.value(QStringLiteral("ok")).toBool()) << c.op;
        EXPECT_EQ(c.resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("unsupported_format"))
            << c.op << ": " << QJsonDocument(c.resp).toJson().toStdString();
        // ANTS-5396 — the refusal names the route that works.
        EXPECT_TRUE(c.resp.value(QStringLiteral("error")).toString()
                        .contains(QStringLiteral("roadmap_migrate")))
            << c.op << " refused without naming roadmap_migrate: "
            << QJsonDocument(c.resp).toJson().toStdString();
    }
    EXPECT_EQ(readAll(root + QStringLiteral("/ROADMAP.md")), before)
        << "a refused op wrote the file behind the store";
    EXPECT_FALSE(itemOf(QStringLiteral("PASS-44-2"), projectId).has_value());
    EXPECT_EQ(itemOf(QStringLiteral("PASS-43-5-B"), projectId)->status,
              QStringLiteral("planned"));
    EXPECT_EQ(itemOf(QStringLiteral("PASS-44-1"), projectId)->body, bodyBefore);
}

// INV-5 — boundary: holds before and after the fix.
TEST(RoadmapPassStoreWrite, Inv5UnmigratedPassProjectStillWritesTheFile) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, /*migrate=*/false);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject r = req(root, QStringLiteral("flip"));
    r[QStringLiteral("id")]        = QStringLiteral("PASS-43-5-B");
    r[QStringLiteral("to_status")] = QStringLiteral("shipped");
    QJsonObject resp;
    {
        RemoteControl rc(nullptr);
        resp = rc.cmdRoadmapLogFlipForTest(r).object();
    }
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("format")).toString(), QStringLiteral("pass-headings"));
    const QString file = QString::fromUtf8(readAll(root + QStringLiteral("/ROADMAP.md")));
    EXPECT_TRUE(file.contains(QStringLiteral(
        "#### Pass 43.5.B Follow-up: the same check on the reply path.\n- **Status**: done")))
        << file.toStdString();
}

// ANTS-5395 — RetroDB's shape: a dated Status line carrying Lanes mid-line,
// and a `---` closing each pass. A flip to shipped with a note must re-date
// the Status line, keep the rest of it, and keep the note inside the item.
TEST(RoadmapPassStoreWrite, Ants5395FlipRedatesAndKeepsTheNoteInside) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, /*migrate=*/true, &projectId, kDated);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject flip = req(root, QStringLiteral("flip"));
    flip[QStringLiteral("id")]        = QStringLiteral("PASS-59-41");
    flip[QStringLiteral("to_status")] = QStringLiteral("shipped");
    flip[QStringLiteral("note")]      = QStringLiteral("Closed in the shutdown hook.");
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(flip).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const auto item = itemOf(QStringLiteral("PASS-59-41"), projectId);
    ASSERT_TRUE(item.has_value());
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    EXPECT_TRUE(item->body.contains(QStringLiteral("(%1). Lanes: cache, shutdown.")
                                        .arg(today)))
        << "the Status line kept its planning date or lost its Lanes:\n"
        << item->body.toStdString();
    const qsizetype noteAt = item->body.indexOf(QStringLiteral("Closed in the shutdown hook."));
    const qsizetype ruleAt = item->body.lastIndexOf(QStringLiteral("---"));
    ASSERT_GE(noteAt, 0) << item->body.toStdString();
    if (ruleAt >= 0)
        EXPECT_LT(noteAt, ruleAt) << "the note landed after the pass's separator:\n"
                                  << item->body.toStdString();
    // The neighbouring pass is untouched.
    EXPECT_TRUE(itemOf(QStringLiteral("PASS-59-42"), projectId)->body.contains(
        QStringLiteral("planned (2026-09-01)")));
}

// ANTS-5395 — the file path's annotate (an unmigrated project) keeps the note
// inside the pass too, above its `---`, by the same rule as the store route.
TEST(RoadmapPassStoreWrite, Ants5395FileAnnotateKeepsTheNoteInside) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, /*migrate=*/false, nullptr, kDated);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject ann = req(root, QStringLiteral("annotate"));
    ann[QStringLiteral("id")]   = QStringLiteral("PASS-59-41");
    ann[QStringLiteral("note")] = QStringLiteral("A note for the first pass.");
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(ann).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QByteArray file = readAll(root + QStringLiteral("/ROADMAP.md"));
    const qsizetype noteAt = file.indexOf("A note for the first pass.");
    const qsizetype ruleAt = file.indexOf("---");
    ASSERT_GE(noteAt, 0) << file.toStdString();
    EXPECT_LT(noteAt, ruleAt) << "the note landed after the first pass's separator:\n"
                              << file.toStdString();
}

// ANTS-5404 — RetroDB's re-test of ANTS-5395: a shipping note is a Resolution
// bullet directly above the Status line, and the Status line keeps the word
// this roadmap uses (`shipped`), not the canonical `done`. The Progress bullet
// after the Status line is untouched.
TEST(RoadmapPassStoreWrite, Ants5404ShippingNoteIsAResolutionAboveStatus) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, /*migrate=*/true, &projectId, kRetro);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject flip = req(root, QStringLiteral("flip"));
    flip[QStringLiteral("id")]        = QStringLiteral("PASS-59-71");
    flip[QStringLiteral("to_status")] = QStringLiteral("shipped");
    flip[QStringLiteral("note")]      = QStringLiteral("Token rotated.");
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(flip).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QString today = QDate::currentDate().toString(Qt::ISODate);
    const QString want = QStringLiteral(
        "- **Resolution** (%1): Token rotated.\n"
        "- **Status**: shipped (%1). Lanes: security, packaging.\n").arg(today);
    const auto item = itemOf(QStringLiteral("PASS-59-71"), projectId);
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(item->body.contains(want)) << item->body.toStdString();
    EXPECT_TRUE(item->body.contains(QStringLiteral(
        "- **Progress** (2026-09-26): the user chose to rotate the token.\n"
        "  Waiting on the user.")))
        << "the Progress bullet after the Status line moved:\n"
        << item->body.toStdString();

    const QString file = QString::fromUtf8(readAll(root + QStringLiteral("/ROADMAP.md")));
    EXPECT_TRUE(file.contains(want)) << file.toStdString();
    EXPECT_FALSE(file.contains(QStringLiteral("**Status**: done")))
        << "the render wrote the canonical keyword over the project's own:\n"
        << file.toStdString();
}

// ANTS-5404 — an annotate note is a Progress bullet at the end of the item,
// above its `---`, not a bare line folded into the bullet before it.
TEST(RoadmapPassStoreWrite, Ants5404AnnotateNoteIsAProgressBullet) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seed(guard, tmp, /*migrate=*/true, &projectId, kRetro);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject ann = req(root, QStringLiteral("annotate"));
    ann[QStringLiteral("id")]   = QStringLiteral("PASS-59-72");
    ann[QStringLiteral("note")] = QStringLiteral("Checked on a second library.");
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(ann).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QString today = QDate::currentDate().toString(Qt::ISODate);
    const auto item = itemOf(QStringLiteral("PASS-59-72"), projectId);
    ASSERT_TRUE(item.has_value());
    const QString want = QStringLiteral(
        "- **Source**: in-session 2026-09-02.\n"
        "- **Progress** (%1): Checked on a second library.").arg(today);
    EXPECT_TRUE(item->body.contains(want)) << item->body.toStdString();
    EXPECT_EQ(item->status, QStringLiteral("shipped"));
}

// ANTS-5404 — the file path's annotate writes the same bullet.
TEST(RoadmapPassStoreWrite, Ants5404FileAnnotateNoteIsABullet) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seed(guard, tmp, /*migrate=*/false, nullptr, kRetro);
    ASSERT_FALSE(root.isEmpty());

    QJsonObject ann = req(root, QStringLiteral("annotate"));
    ann[QStringLiteral("id")]   = QStringLiteral("PASS-59-72");
    ann[QStringLiteral("note")] = QStringLiteral("Checked on a second library.");
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogFlipForTest(ann).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    const QString today = QDate::currentDate().toString(Qt::ISODate);
    const QString file = QString::fromUtf8(readAll(root + QStringLiteral("/ROADMAP.md")));
    EXPECT_TRUE(file.contains(QStringLiteral(
        "- **Source**: in-session 2026-09-02.\n"
        "- **Progress** (%1): Checked on a second library.\n").arg(today)))
        << file.toStdString();
}
