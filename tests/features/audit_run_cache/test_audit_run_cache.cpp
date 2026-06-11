// Feature-conformance test for spec.md (ANTS-1555).
//
// Mix of source-scrape (wiring invariants) + in-process exercise of
// the `AuditCache` module (manifest + reaper behaviour). The
// end-to-end `runAudit` path needs external tools on PATH, so this
// test stops at the `recordRun` boundary.

#include <gtest/gtest.h>

#include "auditcache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Write a JSON object to <root>/.audit_cache/index.json. Used to
// seed a known-state manifest for reaper / version / priorRun tests.
bool seedManifest(const QString &root, const QJsonObject &obj) {
    const QString dir = root + QLatin1String("/.audit_cache");
    if (!QDir().mkpath(dir)) return false;
    const QString path = dir + QLatin1String("/index.json");
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) return false;
    const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (sf.write(body) != body.size()) return false;
    return sf.commit();
}

// Touch a file at path; create the parent dir if needed. Used to
// seed sarif sentinels alongside history entries.
bool touch(const QString &path) {
    const QString parent = QFileInfo(path).absolutePath();
    QDir().mkpath(parent);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write("{\"runs\":[]}");
    f.close();
    return true;
}

}  // namespace

// ─────────────────────── Source-scrape invariants ──

TEST(AuditRunCache, EngineHeaderDeclaresCachePathAndPriorRun) {
    const std::string hdr = slurp(SRC_AUDITRUNNER_H_PATH);
    ASSERT_FALSE(hdr.empty());

    // INV-2 — RunResult carries cachePath + priorRun.
    EXPECT_TRUE(contains(hdr, "QString                    cachePath"))
        << "RunResult.cachePath field missing";
    EXPECT_TRUE(contains(hdr, "QJsonObject                priorRun"))
        << "RunResult.priorRun field missing";
    EXPECT_TRUE(contains(hdr, "ANTS-1555"))
        << "RunResult.cachePath/priorRun should reference ANTS-1555";
}

TEST(AuditRunCache, AuditRunnerRoutesThroughAuditCache) {
    const std::string src = slurp(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-1 — auditrunner.cpp calls into AuditCache::sarifPathFor +
    // AuditCache::recordRun on the success path.
    EXPECT_TRUE(contains(src, "#include \"auditcache.h\""))
        << "auditrunner.cpp must include auditcache.h";
    EXPECT_TRUE(contains(src, "AuditCache::sarifPathFor"))
        << "auditrunner.cpp must call sarifPathFor for the cache path";
    EXPECT_TRUE(contains(src, "AuditCache::recordRun"))
        << "auditrunner.cpp must call recordRun at the end of runAudit";
    EXPECT_TRUE(contains(src, "AuditCache::isoNow"))
        << "auditrunner.cpp must call isoNow for the manifest timestamp";
    EXPECT_TRUE(contains(src, "AuditCache::gitInfo"))
        << "auditrunner.cpp must call gitInfo for the manifest sha/branch";
    // /tmp fallback must remain reachable for read-only roots.
    EXPECT_TRUE(contains(src, "allocSarifPath()"))
        << "auditrunner.cpp must retain the /tmp fallback via allocSarifPath";
}

TEST(AuditRunCache, MainWindowEnvelopeSurfacesCachePathAndPriorRun) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());

    // Locate the audit_run provider lambda.
    const auto reg = mw.find("registerToolProvider(\"audit_run\"");
    ASSERT_NE(reg, std::string::npos);
    // Window widened 6500 → 7200 for ANTS-2032's partial/incomplete_tools
    // envelope block, which lands before the cache_path/prior_run lines.
    const std::string region = mw.substr(reg, 7200);

    EXPECT_TRUE(contains(region, "\"cache_path\""))
        << "audit_run envelope must emit cache_path";
    EXPECT_TRUE(contains(region, "\"prior_run\""))
        << "audit_run envelope must emit prior_run";
    EXPECT_TRUE(contains(region, "r.cachePath"))
        << "envelope must read r.cachePath from RunResult";
    EXPECT_TRUE(contains(region, "r.priorRun"))
        << "envelope must read r.priorRun from RunResult";
}

TEST(AuditRunCache, DescriptorMentionsAuditCacheRouting) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());

    // Locate audit_run descriptor block.
    const auto name = ci.find("t[\"name\"] = \"audit_run\"");
    ASSERT_NE(name, std::string::npos);
    const auto end = ci.find("tools.append(t);", name);
    ASSERT_NE(end, std::string::npos);
    const std::string block = ci.substr(name, end - name);

    // Descriptor names the cache routing + the new fields so callers
    // can discover them without reading the spec.
    EXPECT_TRUE(contains(block, ".audit_cache"))
        << "audit_run description must mention .audit_cache routing";
    EXPECT_TRUE(contains(block, "cache_path"))
        << "audit_run description must mention cache_path envelope field";
    EXPECT_TRUE(contains(block, "prior_run"))
        << "audit_run description must mention prior_run envelope field";
    EXPECT_TRUE(contains(block, "ANTS-1555"))
        << "audit_run description must cite ANTS-1555";
}

TEST(AuditRunCache, AuditCacheUsesAtomicWriteWithOwnerPerms) {
    const std::string src = slurp(SRC_AUDITCACHE_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-6 / INV-10 — QSaveFile + setOwnerOnlyPerms wrap the manifest write.
    EXPECT_TRUE(contains(src, "QSaveFile"))
        << "auditcache must persist via QSaveFile (atomic write)";
    EXPECT_TRUE(contains(src, "setOwnerOnlyPerms"))
        << "auditcache must call setOwnerOnlyPerms for 0600 manifest";
    EXPECT_TRUE(contains(src, "commit()"))
        << "auditcache must call QSaveFile::commit()";
}

TEST(AuditRunCache, ReaperRunsAfterManifestCommit) {
    // ANTS-2004 — the retention reaper must delete dropped SARIF/HTML
    // files only AFTER QSaveFile::commit() succeeds. Deleting first
    // meant a commit failure left files gone but the old manifest still
    // referencing them (permanent orphan refs). Enforce the ordering at
    // the source level: commit() precedes the reaper loop.
    const std::string src = slurp(SRC_AUDITCACHE_CPP_PATH);
    ASSERT_FALSE(src.empty());

    const auto commitPos = src.find("sf.commit()");
    const auto reaperPos = src.find("Reaper (INV-4)");
    ASSERT_NE(commitPos, std::string::npos) << "commit() call not found";
    ASSERT_NE(reaperPos, std::string::npos) << "reaper block not found";
    EXPECT_LT(commitPos, reaperPos)
        << "ANTS-2004: reaper must run after the manifest is committed";
}

// ─────────────────────── In-process module exercise ──

TEST(AuditRunCache, CacheDirPathDerivesFromCanonProject) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    const QString cd = AuditCache::cacheDir(root);
    EXPECT_EQ(cd, root + QStringLiteral("/.audit_cache"));
}

TEST(AuditRunCache, SarifPathForBuildsExpectedFilename) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    const QString p = AuditCache::sarifPathFor(root,
        QStringLiteral("2026-05-18T18-32-00Z"),
        QStringLiteral("acdce78"));
    EXPECT_EQ(p,
        root + QStringLiteral("/.audit_cache/audit-2026-05-18T18-32-00Z-acdce78.sarif"));
}

TEST(AuditRunCache, IsoNowEncodesUTCWithFilesystemSafeColons) {
    const AuditCache::IsoNow now = AuditCache::isoNow();
    // forManifest carries the canonical "T..:..:..Z" form.
    EXPECT_TRUE(now.forManifest.contains(QChar('T')));
    EXPECT_TRUE(now.forManifest.endsWith(QChar('Z')));
    EXPECT_TRUE(now.forManifest.contains(QChar(':')));
    // forFilename is colon-free.
    EXPECT_FALSE(now.forFilename.contains(QChar(':')));
    // Filename form has hyphens where the manifest form has colons.
    EXPECT_EQ(now.forFilename.length(), now.forManifest.length());
}

TEST(AuditRunCache, LoadManifestTreatsUnknownVersionAsEmpty) {
    // INV-7 — version != 1 → empty Manifest.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    QJsonObject seed;
    seed[QStringLiteral("version")] = 99;
    QJsonObject lr;
    lr[QStringLiteral("iso_timestamp")] = QStringLiteral("2026-01-01T00:00:00Z");
    lr[QStringLiteral("commit")] = QStringLiteral("deadbee");
    seed[QStringLiteral("last_run")] = lr;
    ASSERT_TRUE(seedManifest(root, seed));

    const AuditCache::Manifest m = AuditCache::loadManifest(root);
    EXPECT_TRUE(m.lastRun.isEmpty())
        << "version != 1 must yield an empty lastRun";
    EXPECT_TRUE(m.raw.isEmpty())
        << "version != 1 must yield an empty raw manifest object";
}

TEST(AuditRunCache, RecordRunSurfacesPriorRunFromManifest) {
    // INV-8 — recordRun surfaces the prior manifest's last_run via
    // priorRunOut.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    QJsonObject priorLastRun;
    priorLastRun[QStringLiteral("iso_timestamp")] =
        QStringLiteral("2026-05-18T18:00:00Z");
    priorLastRun[QStringLiteral("commit")] = QStringLiteral("aaa1111");
    priorLastRun[QStringLiteral("sarif")] =
        QStringLiteral("audit-2026-05-18T18-00-00Z-aaa1111.sarif");
    QJsonObject seed;
    seed[QStringLiteral("version")] = 1;
    seed[QStringLiteral("last_run")] = priorLastRun;
    ASSERT_TRUE(seedManifest(root, seed));

    QJsonObject newLastRun;
    newLastRun[QStringLiteral("iso_timestamp")] =
        QStringLiteral("2026-05-18T19:00:00Z");
    newLastRun[QStringLiteral("commit")] = QStringLiteral("bbb2222");
    newLastRun[QStringLiteral("sarif")] =
        QStringLiteral("audit-2026-05-18T19-00-00Z-bbb2222.sarif");

    QJsonObject prior;
    const AuditCache::RecordedRun rec =
        AuditCache::recordRun(root, newLastRun, &prior);
    EXPECT_TRUE(rec.ok);
    EXPECT_EQ(prior.value(QStringLiteral("iso_timestamp")).toString(),
              QStringLiteral("2026-05-18T18:00:00Z"));
    EXPECT_EQ(prior.value(QStringLiteral("commit")).toString(),
              QStringLiteral("aaa1111"));
}

TEST(AuditRunCache, RecordRunCapsHistoryAtTenAndReapsDroppedFiles) {
    // INV-3 + INV-4 — history capped at 10 entries; dropped entries'
    // sarif files reaped.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    // Seed manifest with 1 last_run (about to become history[0]) + 10
    // history entries (a total of 11 once last_run migrates), plus
    // sarif sentinel files for each. After recordRun, history caps
    // at 10 and the oldest entry's sarif is reaped.
    QJsonObject prevLastRun;
    prevLastRun[QStringLiteral("iso_timestamp")] =
        QStringLiteral("2026-05-18T11:00:00Z");
    prevLastRun[QStringLiteral("commit")] = QStringLiteral("prev0000");
    prevLastRun[QStringLiteral("sarif")] =
        QStringLiteral("audit-2026-05-18T11-00-00Z-prev0000.sarif");
    ASSERT_TRUE(touch(root + QStringLiteral("/.audit_cache/")
                      + prevLastRun.value(QStringLiteral("sarif")).toString()));

    QJsonArray history;
    QString oldestSarif;
    for (int i = 9; i >= 0; --i) {
        QJsonObject e;
        const QString iso = QStringLiteral("2026-05-18T0%1:00:00Z").arg(i);
        const QString commit = QStringLiteral("hist%1000").arg(i);
        const QString sarif = QStringLiteral("audit-2026-05-18T0%1-00-00Z-%2.sarif")
                                  .arg(i).arg(commit);
        e[QStringLiteral("iso_timestamp")] = iso;
        e[QStringLiteral("commit")]        = commit;
        e[QStringLiteral("sarif")]         = sarif;
        history.append(e);
        ASSERT_TRUE(touch(root + QStringLiteral("/.audit_cache/") + sarif));
        if (i == 0) oldestSarif = sarif;  // newest-first in history[]
    }
    // After we record one new run, prevLastRun migrates to history[0]
    // and oldestSarif (currently history[9]) drops off the end.

    QJsonObject seed;
    seed[QStringLiteral("version")]  = 1;
    seed[QStringLiteral("last_run")] = prevLastRun;
    seed[QStringLiteral("history")]  = history;
    ASSERT_TRUE(seedManifest(root, seed));

    // Pre-seed a FOREIGN file (auditdialog GUI naming) to verify the
    // reaper doesn't touch it.
    const QString foreign = root +
        QStringLiteral("/.audit_cache/audit-20260101-000000.sarif");
    ASSERT_TRUE(touch(foreign));
    // ... and trend.json + baseline.json (also written by the GUI).
    const QString trend = root + QStringLiteral("/.audit_cache/trend.json");
    const QString baseline = root + QStringLiteral("/.audit_cache/baseline.json");
    ASSERT_TRUE(touch(trend));
    ASSERT_TRUE(touch(baseline));

    QJsonObject newLastRun;
    newLastRun[QStringLiteral("iso_timestamp")] =
        QStringLiteral("2026-05-18T20:00:00Z");
    newLastRun[QStringLiteral("commit")] = QStringLiteral("new00000");
    newLastRun[QStringLiteral("sarif")]  =
        QStringLiteral("audit-2026-05-18T20-00-00Z-new00000.sarif");
    QJsonObject prior;
    const AuditCache::RecordedRun rec =
        AuditCache::recordRun(root, newLastRun, &prior);
    ASSERT_TRUE(rec.ok);

    // Re-load the manifest and check history shape.
    const AuditCache::Manifest after = AuditCache::loadManifest(root);
    ASSERT_FALSE(after.raw.isEmpty());
    const QJsonArray hist =
        after.raw.value(QStringLiteral("history")).toArray();
    EXPECT_EQ(hist.size(), 10)
        << "INV-3: history[] must be capped at 10 entries";

    // INV-4: dropped sarif file (the oldest history[9] entry) gone.
    EXPECT_FALSE(QFile::exists(root + QStringLiteral("/.audit_cache/")
                               + oldestSarif))
        << "INV-4: dropped history entry's sarif must be reaped";
    // Foreign files survive (INV-4 + INV-9).
    EXPECT_TRUE(QFile::exists(foreign))
        << "INV-9: foreign sarif files (GUI naming) must survive";
    EXPECT_TRUE(QFile::exists(trend))
        << "INV-9: trend.json must never be reaped";
    EXPECT_TRUE(QFile::exists(baseline))
        << "INV-9: baseline.json must never be reaped";

    // Prior run surfaced.
    EXPECT_EQ(prior.value(QStringLiteral("commit")).toString(),
              QStringLiteral("prev0000"));
}

TEST(AuditRunCache, RecordRunPersistsManifestWithOwnerOnlyPerms) {
    // INV-6 — manifest written with 0600 perms.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    QJsonObject newLastRun;
    newLastRun[QStringLiteral("iso_timestamp")] =
        QStringLiteral("2026-05-18T22:00:00Z");
    newLastRun[QStringLiteral("commit")] = QStringLiteral("ccc3333");
    newLastRun[QStringLiteral("sarif")]  =
        QStringLiteral("audit-2026-05-18T22-00-00Z-ccc3333.sarif");
    QJsonObject prior;
    const AuditCache::RecordedRun rec =
        AuditCache::recordRun(root, newLastRun, &prior);
    ASSERT_TRUE(rec.ok);

    const QString manifestPath =
        root + QStringLiteral("/.audit_cache/index.json");
    QFileInfo fi(manifestPath);
    ASSERT_TRUE(fi.exists()) << "manifest must be written";
    // QFile::Permissions on Linux maps the underlying mode.
    const QFile::Permissions perms = fi.permissions();
    const QFile::Permissions ownerOnly =
        QFile::ReadOwner | QFile::WriteOwner;
    // Strip flags that some filesystems silently add (umask vs.
    // explicit mode); the assertion is "no group/world access".
    const QFile::Permissions groupWorld =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
      | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    EXPECT_EQ(perms & groupWorld, QFile::Permissions{})
        << "manifest must not have group/world bits";
    EXPECT_TRUE((perms & ownerOnly) == ownerOnly)
        << "manifest must have owner read+write";
}

