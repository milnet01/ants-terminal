// Feature-conformance test for ANTS-1359 verify_changes build-cache.
// Spec: tests/features/verify_changes_build_cache/spec.md +
// docs/specs/ANTS-1359.md.

#include "remotecontrol.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>


#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif

namespace {

// ---- Helpers ----------------------------------------------------------


bool runGit(const QString &dir, const QStringList &argv) {
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"), argv);
    if (!p.waitForFinished(5000)) {
        p.kill();
        p.waitForFinished(500);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// Initialise `dir` as a git repo with one committed file. Author /
// committer locally set so the test doesn't depend on a user-level
// gitconfig.
bool initGitProject(const QString &dir) {
    if (!runGit(dir, {QStringLiteral("init"), QStringLiteral("-q"),
                      QStringLiteral("-b"), QStringLiteral("main")})) {
        return false;
    }
    if (!runGit(dir, {QStringLiteral("config"),
                      QStringLiteral("user.email"),
                      QStringLiteral("test@example.com")})) return false;
    if (!runGit(dir, {QStringLiteral("config"),
                      QStringLiteral("user.name"),
                      QStringLiteral("Test")})) return false;
    QFile seed(QDir(dir).filePath(QStringLiteral("src/foo.cpp")));
    QDir().mkpath(QFileInfo(seed).absolutePath());
    if (!seed.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    seed.write("int foo() { return 0; }\n");
    seed.close();
    if (!runGit(dir, {QStringLiteral("add"),
                      QStringLiteral("src/foo.cpp")})) return false;
    return runGit(dir, {QStringLiteral("commit"), QStringLiteral("-q"),
                        QStringLiteral("-m"), QStringLiteral("init")});
}

void writeVerifyJson(const QString &dir, const QByteArray &body) {
    const QString p = QDir(dir).filePath(QStringLiteral(".ants/verify.json"));
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

QJsonObject argsForRoot(const QString &root) {
    QJsonObject o;
    o[QStringLiteral("caller_cwd")] = root;
    return o;
}

QJsonObject runVerify(RemoteControl &rc, const QString &root,
                      const QJsonObject &extra = {}) {
    QJsonObject args = argsForRoot(root);
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        args[it.key()] = it.value();
    }
    return rc.cmdVerifyChangesWithRoot(root, args).object();
}

}  // namespace

// ---------------------------------------------------------------------------
// Direct cache-API tests — INV-1, INV-2, INV-6, INV-11.
// ---------------------------------------------------------------------------

// INV-1: hit returns byte-identical response (plus cache_hit will be
// added by the dispatch wrapper; the direct API stores the pre-wrap
// response and returns it).
TEST(VerifyChangesBuildCache, DirectApiPutGetRoundTrip) {
    RemoteControl rc(nullptr);
    QJsonObject resp;
    resp[QStringLiteral("ok")] = true;
    resp[QStringLiteral("all_passed")] = true;
    resp[QStringLiteral("marker")] = QStringLiteral("alpha");
    rc.putVerifyCacheForTest(QStringLiteral("k0"), resp);
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 1);
    const QJsonObject got = rc.tryGetVerifyCacheForTest(QStringLiteral("k0"));
    EXPECT_EQ(got.value(QStringLiteral("marker")).toString(),
              QStringLiteral("alpha"));
}

// INV-6: cap at 8; insert of a 9th distinct key evicts the LRU entry.
TEST(VerifyChangesBuildCache, LruEvictionAtCap) {
    RemoteControl rc(nullptr);
    for (int i = 0; i < 9; ++i) {
        QJsonObject r;
        r[QStringLiteral("i")] = i;
        rc.putVerifyCacheForTest(
            QStringLiteral("k%1").arg(i), r);
    }
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 8)
        << "cap should clamp size to 8";
    // The first-inserted key (k0) is the LRU and should have been
    // evicted; k8 is most recent so it stays.
    const QStringList lru = rc.verifyCacheLruForTest();
    EXPECT_FALSE(lru.contains(QStringLiteral("k0")))
        << "LRU entry k0 should have been evicted";
    EXPECT_TRUE(lru.contains(QStringLiteral("k8")))
        << "most-recently inserted key should still be present";
}

// INV-6 (HitBumpsToMru): putting an existing key bumps it to MRU.
TEST(VerifyChangesBuildCache, ReinsertBumpsToMru) {
    RemoteControl rc(nullptr);
    QJsonObject r;
    rc.putVerifyCacheForTest(QStringLiteral("kA"), r);
    rc.putVerifyCacheForTest(QStringLiteral("kB"), r);
    rc.putVerifyCacheForTest(QStringLiteral("kC"), r);
    // Re-insert kA — should move it to MRU position.
    rc.putVerifyCacheForTest(QStringLiteral("kA"), r);
    const QStringList lru = rc.verifyCacheLruForTest();
    ASSERT_FALSE(lru.isEmpty());
    EXPECT_EQ(lru.first(), QStringLiteral("kA"))
        << "kA should have been bumped to MRU";
}

// INV-11: reentrancy gate. Set the flag manually; a cmdVerifyChanges
// call returns verify_in_flight immediately.
TEST(VerifyChangesBuildCache, ReentrancyRejectedWhenFlagSet) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    rc.setVerifyInFlightForTest(true);
    const QJsonObject env = runVerify(rc, tmp.path());
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool(true));
    EXPECT_EQ(env.value(QStringLiteral("error")).toString(),
              QStringLiteral("verify_in_flight"));
    // Flag still set — no RAII triggered because the call short-
    // circuits before the guard is installed (early-return path).
    rc.setVerifyInFlightForTest(false);
}

// ---------------------------------------------------------------------------
// Behavioural tests — INV-4 (exclusions), INV-5 (invalidation),
// INV-8 / INV-9 (control flags). Each uses a fresh QTemporaryDir
// git project with /bin/true gates so the build is sub-ms.
// ---------------------------------------------------------------------------

// First call runs gates, second call within TTL hits the cache.
TEST(VerifyChangesBuildCache, HitWithinTtl) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"},
        "tests": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    const QJsonObject a = runVerify(rc, tmp.path());
    EXPECT_TRUE(a.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(a.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 1);

    const QJsonObject b = runVerify(rc, tmp.path());
    EXPECT_TRUE(b.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(b.value(QStringLiteral("cache_hit")).toBool());
}

// INV-5: tracked-file edit between calls busts the cache.
TEST(VerifyChangesBuildCache, TrackedEditBustsCache) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    (void)runVerify(rc, tmp.path());
    ASSERT_EQ(rc.verifyCacheSizeForTest(), 1);

    // Modify the tracked file's content (mtime+content change shows
    // in git status --porcelain).
    QFile f(QDir(tmp.path()).filePath(QStringLiteral("src/foo.cpp")));
    ASSERT_TRUE(f.open(QIODevice::Append));
    f.write("// tweak\n");
    f.close();

    const QJsonObject b = runVerify(rc, tmp.path());
    EXPECT_FALSE(b.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 2)
        << "tracked edit should produce a fresh cache entry";
}

// INV-5: untracked new file (not gitignored) busts the cache.
TEST(VerifyChangesBuildCache, UntrackedFileBustsCache) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    (void)runVerify(rc, tmp.path());
    ASSERT_EQ(rc.verifyCacheSizeForTest(), 1);

    QFile nf(QDir(tmp.path()).filePath(QStringLiteral("new.cpp")));
    ASSERT_TRUE(nf.open(QIODevice::WriteOnly));
    nf.write("int x;\n");
    nf.close();

    const QJsonObject b = runVerify(rc, tmp.path());
    EXPECT_FALSE(b.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 2);
}

// INV-5: an empty commit changes HEAD and busts the cache.
TEST(VerifyChangesBuildCache, CommitBustsCache) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    (void)runVerify(rc, tmp.path());
    ASSERT_EQ(rc.verifyCacheSizeForTest(), 1);

    ASSERT_TRUE(runGit(tmp.path(),
        {QStringLiteral("commit"), QStringLiteral("-q"),
         QStringLiteral("--allow-empty"), QStringLiteral("-m"),
         QStringLiteral("empty")}));

    const QJsonObject b = runVerify(rc, tmp.path());
    EXPECT_FALSE(b.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 2);
}

// INV-4 class 1: bad_config never inserted.
TEST(VerifyChangesBuildCache, BadConfigNotCached) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), "{ this is not json");

    RemoteControl rc(nullptr);
    const QJsonObject env = runVerify(rc, tmp.path());
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool(true));
    EXPECT_EQ(env.value(QStringLiteral("error")).toString(),
              QStringLiteral("bad_config"));
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 0)
        << "bad_config should never insert into the cache";
}

// INV-4 class 2: cfgSource == "none" not inserted (no .ants/, no
// build system → no gates ran).
TEST(VerifyChangesBuildCache, NoneConfigNotCached) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));

    RemoteControl rc(nullptr);
    const QJsonObject env = runVerify(rc, tmp.path());
    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("config_source")).toString(),
              QStringLiteral("none"));
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 0)
        << "config_source=none should never insert";
}

// INV-4 class 5: non-git project bypasses cache entirely.
TEST(VerifyChangesBuildCache, NonGitNotCached) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // No git init — just a verify.json.
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    const QJsonObject env = runVerify(rc, tmp.path());
    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(env.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 0)
        << "non-git project should bypass cache";
}

// Note: INV-4 class 6 first-half ("command not resolvable") only fires
// when QProcess fails to start /bin/sh itself (verifyengine.cpp:296-302).
// A bogus *command* under sh -c produces ran:true, passed:false,
// exitCode:127 — that's a deterministic test failure and IS cached
// (covered by TestFailureIsCached below). The /bin/sh-missing case is
// unreachable from a user-side .ants/verify.json on a normal system,
// so no behavioural test for it; the exclusion is structurally correct
// in the code path.

// INV-1: failing tests over identical inputs ARE cached.
TEST(VerifyChangesBuildCache, TestFailureIsCached) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"},
        "tests": {"command": "/bin/false", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    const QJsonObject a = runVerify(rc, tmp.path());
    EXPECT_FALSE(a.value(QStringLiteral("all_passed")).toBool(true));
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 1)
        << "deterministic failure over identical inputs should be cached";

    const QJsonObject b = runVerify(rc, tmp.path());
    EXPECT_TRUE(b.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_FALSE(b.value(QStringLiteral("all_passed")).toBool(true));
}

// INV-8: force_refresh bypasses lookup.
TEST(VerifyChangesBuildCache, ForceRefreshBypassesLookup) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    (void)runVerify(rc, tmp.path());
    ASSERT_EQ(rc.verifyCacheSizeForTest(), 1);

    QJsonObject extra;
    extra[QStringLiteral("force_refresh")] = true;
    const QJsonObject b = runVerify(rc, tmp.path(), extra);
    EXPECT_FALSE(b.value(QStringLiteral("cache_hit")).toBool())
        << "force_refresh must bypass the lookup";
    // The entry should be re-inserted (same key) — size stays at 1.
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 1);
}

// INV-9 (cache_only miss): probe before any cache entry returns
// cache_miss without running gates.
TEST(VerifyChangesBuildCache, CacheOnlyProbeMiss) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    QJsonObject extra;
    extra[QStringLiteral("cache_only")] = true;
    const QJsonObject env = runVerify(rc, tmp.path(), extra);
    EXPECT_TRUE(env.value(QStringLiteral("ok")).toBool());
    EXPECT_FALSE(env.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_TRUE(env.value(QStringLiteral("cache_miss")).toBool());
    EXPECT_FALSE(env.contains(QStringLiteral("gates")))
        << "cache_only miss must not include a gates field";
}

// INV-9 (cache_only hit): probe AFTER an entry exists returns the
// cached response.
TEST(VerifyChangesBuildCache, CacheOnlyProbeHit) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    (void)runVerify(rc, tmp.path());

    QJsonObject extra;
    extra[QStringLiteral("cache_only")] = true;
    const QJsonObject env = runVerify(rc, tmp.path(), extra);
    EXPECT_TRUE(env.value(QStringLiteral("cache_hit")).toBool());
    EXPECT_TRUE(env.contains(QStringLiteral("gates")));
}

// INV-9 (combo): force_refresh + cache_only together → incompatible_args.
TEST(VerifyChangesBuildCache, ForceAndCacheOnlyIncompatible) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(initGitProject(tmp.path()));
    writeVerifyJson(tmp.path(), R"({
        "build": {"command": "/bin/true", "format": "plain"}
    })");

    RemoteControl rc(nullptr);
    QJsonObject extra;
    extra[QStringLiteral("force_refresh")] = true;
    extra[QStringLiteral("cache_only")]    = true;
    const QJsonObject env = runVerify(rc, tmp.path(), extra);
    EXPECT_FALSE(env.value(QStringLiteral("ok")).toBool(true));
    EXPECT_EQ(env.value(QStringLiteral("error")).toString(),
              QStringLiteral("incompatible_args"));
    EXPECT_EQ(rc.verifyCacheSizeForTest(), 0);
}

// ---------------------------------------------------------------------------
// Source-grep — cap constant.
// ---------------------------------------------------------------------------

TEST(VerifyChangesBuildCache, CapConstantDeclared) {
    const std::string h = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(h.empty());
    EXPECT_NE(h.find("kVerifyCacheCap   = 8"), std::string::npos)
        << "kVerifyCacheCap = 8 declaration missing in remotecontrol.h";
}
