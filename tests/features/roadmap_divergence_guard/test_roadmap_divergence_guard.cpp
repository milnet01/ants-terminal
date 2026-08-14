// Feature-conformance test for ANTS-4141 part 1 — the divergence guard in
// RoadmapWrite::commitAndRender(). Contract:
// tests/features/roadmap_divergence_guard/spec.md
//
// Behavioural, through the roadmap_log verbs: each case migrates a small
// markdown fixture into a store at RoadmapStore::defaultPath() (redirected into
// the case's sandbox), writes a bullet into ROADMAP.md BY HAND so the store has
// never seen it — the way the real ~200-id divergence was produced — and then
// drives a verb and asserts what the file and the store hold afterwards.

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

bool appendToFile(const QString &path, const QByteArray &body) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
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
// — below it the write paths will not trust an ants-v1 walk and refuse
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

// Two items, both with a `Layman:` line so the render's INV-5 gate is clear and
// the only refusal a case can hit is the one under test.
QByteArray fixture() {
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
         "  Source: seed.\n";
    return b;
}

// The hand edit, written AFTER the migration so the store has never seen it.
// This is the shape ANTS-4065 D3 measured: a well-formed bullet, filed by hand
// between two verb calls, which the render then has no row for.
const char *kHandFiled =
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0099] **A bullet filed by hand.**\n"
    "  Layman: The store has never seen this.\n"
    "  Kind: fix.\n"
    "  Source: hand.\n";

// An id-less bullet whose PROSE cross-references an id the render does not
// emit — a retired one, an `internal` / `dropped` one (excluded by the render's
// INV-4), one that only ever existed in an archive. Roadmap prose is full of
// these. BulletRecord::id would take the mention, because it is positionless
// and takes the first bracketed token ANYWHERE in the body; idToken, which the
// guard reads, is empty, because the bullet owns no id.
const char *kIdlessMentioning =
    "\n"
    "- \xF0\x9F\x93\x8B **A bullet with no id of its own.**\n"
    "  Layman: It merely cross-references [DEMO-0404] in passing.\n"
    "  Kind: chore.\n"
    "  Source: hand.\n";

// Redirect XDG_DATA_HOME into the sandbox, write the fixture, and migrate it.
// Bulk, because RoadmapMigrateLoad::load() refuses an Interactive connection
// (ANTS-3765 INV-12); closed before the verb under test runs.
QString seedMigrated(ants_test::XdgGuard &guard, const QTemporaryDir &tmp,
                     qint64 *projectId) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString rawRoot = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(rawRoot + QStringLiteral("/ROADMAP.md"), fixture()))
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
    opts.changedAt   = QStringLiteral("2026-08-14T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return QString();
    }
    *projectId = out.projectId;
    return root;
}

QJsonObject flipReq(const QString &root) {
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("op")]         = QStringLiteral("flip");
    req[QStringLiteral("to_status")]  = QStringLiteral("shipped");
    req[QStringLiteral("id")]         = QStringLiteral("DEMO-0007");
    return req;
}

QJsonObject runFlip(const QJsonObject &req) {
    RemoteControl rc(nullptr);
    return rc.cmdRoadmapLogFlipForTest(req).object();
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

// The whole point: a render that would delete a bullet the store never imported
// refuses instead of publishing, and nothing moves on either side.
TEST(RoadmapDivergenceGuard, RefusesRatherThanDropping) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QString roadmap = root + QStringLiteral("/ROADMAP.md");
    ASSERT_TRUE(appendToFile(roadmap, QByteArray(kHandFiled)));
    const QByteArray before = readAll(roadmap);

    const QJsonObject resp = runFlip(flipReq(root));

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_would_drop"));
    // Naming the id is the whole remedy path: the caller has to know WHICH
    // bullets the store is missing before it can import them.
    EXPECT_TRUE(resp.value(QStringLiteral("error")).toString()
                    .contains(QStringLiteral("DEMO-0099")))
        << resp.value(QStringLiteral("error")).toString().toStdString();

    EXPECT_EQ(readAll(roadmap), before)
        << "the refusal must not have rewritten the file";
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("planned"))
        << "a store write must not survive the render that refused it";
}

// ---------------------------------------------------------------- INV-2 -----

// A preview reports the refusal a real call would hit. The gate above it
// already behaves this way; a preview that previewed a publish the real call
// refuses would be lying about the one thing it is for.
TEST(RoadmapDivergenceGuard, DryRunRefusesToo) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    ASSERT_TRUE(appendToFile(root + QStringLiteral("/ROADMAP.md"),
                             QByteArray(kHandFiled)));

    QJsonObject req = flipReq(root);
    req[QStringLiteral("dry_run")] = true;
    const QJsonObject resp = runFlip(req);

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("render_would_drop"));
}

// ---------------------------------------------------------------- INV-3 -----

// The guard sits on the path EVERY write takes, so a false refusal is a broken
// write path. With no hand edit the store is a superset of the file and the
// flip publishes as it always did.
TEST(RoadmapDivergenceGuard, ConvergedStoreStillPublishes) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    const QJsonObject resp = runFlip(flipReq(root));

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("shipped"));
    EXPECT_TRUE(readAll(root + QStringLiteral("/ROADMAP.md"))
                    .contains("\xE2\x9C\x85 [DEMO-0007]"));
}

// ---------------------------------------------------------------- INV-4 -----

// An id-less bullet that merely MENTIONS a live id does not own it. The guard
// reads the leading id slot; BulletRecord::id would take the mention and refuse
// a render that drops nothing.
TEST(RoadmapDivergenceGuard, MentionInBodyIsNotOwnership) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qint64 projectId = 0;
    const QString root = seedMigrated(guard, tmp, &projectId);
    ASSERT_FALSE(root.isEmpty());

    ASSERT_TRUE(appendToFile(root + QStringLiteral("/ROADMAP.md"),
                             QByteArray(kIdlessMentioning)));

    const QJsonObject resp = runFlip(flipReq(root));

    EXPECT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << resp.value(QStringLiteral("error")).toString().toStdString();
    EXPECT_EQ(statusOf(QStringLiteral("DEMO-0007"), projectId),
              QStringLiteral("shipped"));
}
