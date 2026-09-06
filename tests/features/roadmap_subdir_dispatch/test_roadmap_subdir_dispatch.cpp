// Feature-conformance test for ANTS-4884 — the store-vs-markdown dispatch
// resolves the project from a subdirectory. See
// tests/features/roadmap_subdir_dispatch/spec.md.
//
// Both dispatchers ask readProjectByRoot(), which keys a project on its
// canonical ROOT. Handed caller_cwd from a subdirectory the lookup matches
// nothing, the dispatcher reads "not migrated", and the verb silently takes the
// markdown path — against a file that is an output of the store.

#include "../../_support/xdg_guard.h"

#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace {

// ants-v1 throughout, which is the one dialect migratedProject() serves from
// the store — a mixed file would take the markdown path at the root too and the
// subdirectory case would prove nothing.
const char *kRoadmap =
    "# Demo Roadmap\n"
    "\n"
    "## To Do\n"
    "\n"
    "- \xF0\x9F\x93\x8B [DEMO-0001] **A seed bullet.**\n"
    "  Layman: A thing.\n"
    "  Kind: chore.\n"
    "  Source: seed.\n";

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
// REAL store under XDG_DATA_HOME. XdgGuard redirects that first.
bool migrate(const QString &root) {
    auto store = std::make_unique<RoadmapStore>(
        RoadmapStore::defaultPath(), RoadmapStore::kDefaultHistoryCapBytes,
        RoadmapStore::Access::Bulk);
    QString err;
    if (!store->open(&err)) {
        ADD_FAILURE() << "store open: " << err.toStdString();
        return false;
    }
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) {
        ADD_FAILURE() << "findRoadmaps: " << err.toStdString();
        return false;
    }
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-06T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(*store, plan, opts);
    if (!out.ok) {
        ADD_FAILURE() << "migration load: " << out.error.toStdString();
        return false;
    }
    return true;
}

// Writes the fixture and returns the CANONICAL root. The .git is what lets the
// ancestor walk leave the subdirectory at all: with no repo boundary
// repoBoundedAncestors returns the caller's own directory and the subdirectory
// case cannot arise.
QString seedProject(ants_test::XdgGuard &guard, const QTemporaryDir &tmp) {
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString raw = QDir(tmp.path()).filePath(QStringLiteral("proj"));
    if (!writeFile(raw + QStringLiteral("/ROADMAP.md"), QByteArray(kRoadmap)))
        return QString();
    if (!writeFile(raw + QStringLiteral("/.roadmap-counter"), QByteArray("1\n")))
        return QString();
    if (!QDir().mkpath(raw + QStringLiteral("/.git")))
        return QString();
    if (!QDir().mkpath(raw + QStringLiteral("/sub")))
        return QString();
    return QFileInfo(raw).canonicalFilePath();
}

QString subOf(const QString &root) {
    return QFileInfo(QDir(root).filePath(QStringLiteral("sub"))).canonicalFilePath();
}

}  // namespace

// ----------------------------------------------------------------- INV-1 ----

TEST(RoadmapSubdirDispatch, Inv1ReadFromASubdirectoryIsServedFromTheStore) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    // `source` is served from m_roadmapCacheSource, a PER-INSTANCE cache keyed
    // on the roadmap's path and mtime — not on caller_cwd. So the control and
    // the subject need separate RemoteControls: run through one and the root
    // call fills the cache with "store" and the subdirectory call reads it back
    // without re-deriving anything, which passes this case while the defect is
    // live.
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = subOf(root);
    RemoteControl subject(nullptr);
    const QJsonObject inSub = subject.cmdRoadmapQueryForTest(req).object();
    ASSERT_TRUE(inSub.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(inSub).toJson().toStdString();

    // The control, on its own instance: from the root this project IS served
    // from the store. Without it a fixture that never reached the store at all
    // would fail the assertion below for an unrelated reason.
    QJsonObject rootReq;
    rootReq[QStringLiteral("caller_cwd")] = root;
    RemoteControl control(nullptr);
    const QJsonObject atRoot = control.cmdRoadmapQueryForTest(rootReq).object();
    ASSERT_TRUE(atRoot.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(atRoot).toJson().toStdString();
    ASSERT_EQ(atRoot.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "fixture is not store-served at the root, so the subdirectory case "
           "proves nothing: " << QJsonDocument(atRoot).toJson().toStdString();

    EXPECT_EQ(inSub.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "the dispatch was asked by caller_cwd, matched no project row, and "
           "read the markdown file the store renders";
}

// ----------------------------------------------------------------- INV-2 ----
//
// The write side, which is the damaging half: ROADMAP.md is an output of the
// store, so a markdown splice from a subdirectory writes an item the store
// never sees and the next store render discards it.
//
// `line` is the discriminator, as it is for ANTS-4493's INV-3: only the
// markdown splice reports the line it wrote at.

TEST(RoadmapSubdirDispatch, Inv2WriteFromASubdirectoryGoesThroughTheStore) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = subOf(root);
    req[QStringLiteral("section")]    = QStringLiteral("to-do");
    req[QStringLiteral("status")]     = QStringLiteral("planned");
    req[QStringLiteral("headline")]   = QStringLiteral("An append from a subdirectory.");
    // Required, not decoration: the render's gate refuses a project whose open
    // items carry no Layman line.
    req[QStringLiteral("layman")]     = QStringLiteral("A plain-language line.");
    req[QStringLiteral("kind")]       = QStringLiteral("chore");
    req[QStringLiteral("source")]     = QStringLiteral("ants-4884-test");

    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapLogAppendForTest(req).object();
    ASSERT_TRUE(resp.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(resp).toJson().toStdString();

    EXPECT_FALSE(resp.contains(QStringLiteral("line")))
        << "the append spliced the markdown file instead of writing to the "
           "store: " << QJsonDocument(resp).toJson().toStdString();
}

// ----------------------------------------------------------------- INV-3 ----
//
// The resolution is additive. A caller with no roadmap at or above it resolves
// to itself, so it refuses exactly as it did before rather than answering with
// some other project's roadmap.

TEST(RoadmapSubdirDispatch, Inv3CallerOutsideAnyProjectStillRefuses) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());
    const QString bare = QDir(tmp.path()).filePath(QStringLiteral("bare"));
    ASSERT_TRUE(QDir().mkpath(bare));

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] =
        QFileInfo(bare).canonicalFilePath();
    RemoteControl rc(nullptr);
    const QJsonObject resp = rc.cmdRoadmapQueryForTest(req).object();

    EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool())
        << "a directory with no roadmap at or above it answered: "
        << QJsonDocument(resp).toJson().toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
              QStringLiteral("no_roadmap_loaded"))
        << QJsonDocument(resp).toJson().toStdString();
}

// --------------------------------------------------------------- ANTS-4887 --
//
// A git WORKTREE of a registered project is a different path with no store row,
// so every roadmap verb fell through to the markdown path — against a
// ROADMAP.md that is generated output. A flip from there returned ok:true with
// write_path "patch": the store would not record it and the next render from
// the main checkout would erase it. Reported by a session using the roadmap as
// the only coordination state between two parallel sessions.
//
// The fixture writes the `.git` FILE git itself writes — `gitdir:
// <main>/.git/worktrees/<name>` — rather than shelling out, which is also how
// the resolution reads it.

TEST(RoadmapSubdirDispatch, Inv4WorktreeResolvesToTheRegisteredMainCheckout) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    // The worktree: its own checkout of the same generated ROADMAP.md, and a
    // `.git` file pointing back into the main checkout's administrative dir.
    const QString wtRaw = QDir(tmp.path()).filePath(QStringLiteral("proj-wt"));
    ASSERT_TRUE(writeFile(wtRaw + QStringLiteral("/ROADMAP.md"),
                          QByteArray(kRoadmap)));
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/.git/worktrees/proj-wt")));
    ASSERT_TRUE(writeFile(
        wtRaw + QStringLiteral("/.git"),
        (QStringLiteral("gitdir: ") + root
         + QStringLiteral("/.git/worktrees/proj-wt\n")).toUtf8()));
    const QString wt = QFileInfo(wtRaw).canonicalFilePath();
    ASSERT_FALSE(wt.isEmpty());

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = wt;
    RemoteControl rc(nullptr);
    const QJsonObject r = rc.cmdRoadmapQueryForTest(req).object();
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(r).toJson().toStdString();

    EXPECT_EQ(r.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"))
        << "the worktree was read as an unregistered project of its own, so "
           "the verb parsed a file the store renders";
}

TEST(RoadmapSubdirDispatch, Inv4WorktreeOfAProjectWithNoRoadmapIsUnchanged) {
    // The redirect must hold only where the main checkout really is the
    // project. Point the back-reference at a directory that holds no roadmap
    // and the worktree must answer for itself, exactly as before.
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    guard.setEnv("XDG_DATA_HOME",
                 QDir(tmp.path()).filePath(QStringLiteral("xdg")).toUtf8());

    const QString bare = QDir(tmp.path()).filePath(QStringLiteral("bare-main"));
    ASSERT_TRUE(QDir().mkpath(bare + QStringLiteral("/.git/worktrees/wt")));

    const QString wtRaw = QDir(tmp.path()).filePath(QStringLiteral("wt"));
    ASSERT_TRUE(writeFile(wtRaw + QStringLiteral("/ROADMAP.md"),
                          QByteArray(kRoadmap)));
    ASSERT_TRUE(writeFile(
        wtRaw + QStringLiteral("/.git"),
        (QStringLiteral("gitdir: ") + bare
         + QStringLiteral("/.git/worktrees/wt\n")).toUtf8()));
    const QString wt = QFileInfo(wtRaw).canonicalFilePath();

    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = wt;
    RemoteControl rc(nullptr);
    const QJsonObject r = rc.cmdRoadmapQueryForTest(req).object();

    // It answers from its own file rather than being redirected to a root that
    // has nothing to answer with.
    EXPECT_TRUE(r.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(r).toJson().toStdString();
    EXPECT_EQ(r.value(QStringLiteral("source")).toString(),
              QStringLiteral("markdown"));
}

// --------------------------------------------------------------- ANTS-4885 --
//
// `source` is served from m_roadmapCacheSource, derived per cache FILL from
// the filling caller's own project resolution, and the cache is keyed on the
// roadmap's path and mtime — not on caller_cwd. Two callers resolving to
// different projects over one file therefore shared an entry, and the second
// was told the first's backend.
//
// That was reachable while the dispatch was keyed on caller_cwd: a root call
// and a subdirectory call resolved differently over the same file. Since
// ANTS-4884 both resolve to the same project, so one instance answering both
// must now agree. This is the case that says so, on ONE RemoteControl —
// INV-1 above deliberately uses two, to keep the cache out of its measurement.

TEST(RoadmapSubdirDispatch, Ants4885OneInstanceAgreesAcrossRootAndSubdirectory) {
    ants_test::XdgGuard guard;
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedProject(guard, tmp);
    ASSERT_FALSE(root.isEmpty());
    ASSERT_TRUE(migrate(root));

    RemoteControl rc(nullptr);

    QJsonObject subReq;
    subReq[QStringLiteral("caller_cwd")] = subOf(root);
    const QJsonObject a = rc.cmdRoadmapQueryForTest(subReq).object();
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool());

    QJsonObject rootReq;
    rootReq[QStringLiteral("caller_cwd")] = root;
    const QJsonObject b = rc.cmdRoadmapQueryForTest(rootReq).object();
    ASSERT_TRUE(b.value(QStringLiteral("ok")).toBool());

    EXPECT_EQ(a.value(QStringLiteral("source")).toString(),
              QStringLiteral("store"));
    EXPECT_EQ(a.value(QStringLiteral("source")).toString(),
              b.value(QStringLiteral("source")).toString())
        << "one instance reported two backends for one roadmap file";
}
