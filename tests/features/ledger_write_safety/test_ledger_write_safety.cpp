// Feature-conformance test for spec.md — the indie-review #7 file-write
// safety bundle (ANTS-1988 private-dir perms + ANTS-1989 RMW lock).
//
// Behavioural (INV-1..INV-3): exercise the real ledger appenders end-to-end
//   (both ledgers live in ants_core_lib, which test_core links).
// Wiring (INV-4/INV-5): grep the audit-side call sites that aren't reachable
//   in-process here, plus re-assert the ledger wiring.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QTemporaryDir>

#include "modelswitchledger.h"
#include "modelnearmissledger.h"

#include <string>

#include <sys/stat.h>

#ifndef SRC_MODELSWITCHLEDGER_CPP_PATH
#error "SRC_MODELSWITCHLEDGER_CPP_PATH compile definition required"
#endif
#ifndef SRC_MODELNEARMISSLEDGER_CPP_PATH
#error "SRC_MODELNEARMISSLEDGER_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITCACHE_CPP_PATH
#error "SRC_AUDITCACHE_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITRUNNER_CPP_PATH
#error "SRC_AUDITRUNNER_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITAUTOFIX_CPP_PATH
#error "SRC_AUDITAUTOFIX_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITFPLEDGER_CPP_PATH
#error "SRC_AUDITFPLEDGER_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITDIALOG_CPP_PATH
#error "SRC_AUDITDIALOG_CPP_PATH compile definition required"
#endif

namespace {

int dirMode(const QString &path) {
    struct stat st{};
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return -1;
    return st.st_mode & 0777;
}


}  // namespace

// INV-1 — the firing ledger's cache dir is born 0700, not 0755.
TEST(LedgerWriteSafety, Inv1SwitchLedgerDirIsPrivate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // A not-yet-existing nested dir so writeLinesAtomic must create it.
    const QString dir  = tmp.path() + QStringLiteral("/ants-cache/x");
    const QString path = dir + QStringLiteral("/model-switch-ledger.jsonl");

    ModelSwitchLedger::Record r;
    r.ts        = QStringLiteral("2026-06-04T10:00:00Z");
    r.sessionId = QStringLiteral("s1");
    r.project   = QStringLiteral("/mnt/proj");
    r.fromTier  = QStringLiteral("opus");
    r.toTier    = QStringLiteral("haiku");
    r.trigger   = QStringLiteral("auto");
    ASSERT_TRUE(ModelSwitchLedger::appendRecord(path, r));

    EXPECT_EQ(dirMode(dir), 0700)
        << "ledger cache dir must be 0700, got 0" << dirMode(dir);
}

// INV-2 — the near-miss ledger's cache dir is likewise 0700.
TEST(LedgerWriteSafety, Inv2NearMissLedgerDirIsPrivate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir  = tmp.path() + QStringLiteral("/nm-cache/y");
    const QString path = dir + QStringLiteral("/nearmiss.jsonl");

    ModelNearMissLedger::Record r;
    r.ts              = QStringLiteral("2026-06-04T10:00:00Z");
    r.sessionId       = QStringLiteral("s1");
    r.project         = QStringLiteral("/mnt/proj");
    r.currentTier     = QStringLiteral("opus");
    r.recommendedTier = QStringLiteral("haiku");
    r.blockedBy       = {QStringLiteral("composer_not_empty")};
    ASSERT_TRUE(ModelNearMissLedger::appendRecord(path, r));

    EXPECT_EQ(dirMode(dir), 0700)
        << "near-miss cache dir must be 0700, got 0" << dirMode(dir);
}

// INV-3 — the lock leaves the happy path intact: two sequential appends both
// survive (lock released between calls) and a <path>.lock sibling exists.
TEST(LedgerWriteSafety, Inv3LockDoesNotBreakHappyPath) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/ledger.jsonl");

    ModelSwitchLedger::Record r;
    r.ts       = QStringLiteral("2026-06-04T10:00:00Z");
    r.project  = QStringLiteral("/mnt/proj");
    r.fromTier = QStringLiteral("opus");
    r.toTier   = QStringLiteral("haiku");
    r.trigger  = QStringLiteral("auto");

    r.sessionId = QStringLiteral("A");
    ASSERT_TRUE(ModelSwitchLedger::appendRecord(path, r));
    r.sessionId = QStringLiteral("B");
    ASSERT_TRUE(ModelSwitchLedger::appendRecord(path, r));

    const QList<ModelSwitchLedger::Record> recs =
        ModelSwitchLedger::readRecords(path);
    ASSERT_EQ(recs.size(), 2) << "both sequential appends must survive";
    EXPECT_TRUE(QFile::exists(path + QStringLiteral(".lock")))
        << "ConfigWriteLock sibling lock file must exist after a write";
}

// INV-4 / INV-5 — wiring greps over the cited source sites.
TEST(LedgerWriteSafety, Inv4Inv5Wiring) {
    const std::string msl = ants_test::slurpFile(SRC_MODELSWITCHLEDGER_CPP_PATH);
    const std::string nml = ants_test::slurpFile(SRC_MODELNEARMISSLEDGER_CPP_PATH);
    const std::string ac  = ants_test::slurpFile(SRC_AUDITCACHE_CPP_PATH);
    const std::string ar  = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    const std::string af  = ants_test::slurpFile(SRC_AUDITAUTOFIX_CPP_PATH);
    const std::string fp  = ants_test::slurpFile(SRC_AUDITFPLEDGER_CPP_PATH);
    const std::string ad  = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);

    // INV-4 — ensurePrivateDir wired; the bare mkpath idiom gone at each site.
    EXPECT_NE(msl.find("ensurePrivateDir(fi.absolutePath())"), std::string::npos);
    EXPECT_EQ(msl.find("QDir().mkpath(fi.absolutePath())"), std::string::npos);
    EXPECT_NE(nml.find("ensurePrivateDir(fi.absolutePath())"), std::string::npos);
    EXPECT_EQ(nml.find("QDir().mkpath(fi.absolutePath())"), std::string::npos);
    EXPECT_NE(ac.find("return ensurePrivateDir(d)"), std::string::npos);
    EXPECT_NE(ar.find("ensurePrivateDir(AuditCache::cacheDir(canonProject))"),
              std::string::npos);
    EXPECT_NE(af.find("ensurePrivateDir(cacheDir)"), std::string::npos);
    EXPECT_NE(fp.find("ensurePrivateDir(dir)"), std::string::npos);
    EXPECT_NE(ad.find("ensurePrivateDir(m_projectPath"), std::string::npos);

    // INV-5 — ConfigWriteLock wired at each RMW site.
    EXPECT_NE(msl.find("ConfigWriteLock lock(path)"), std::string::npos);
    EXPECT_NE(nml.find("ConfigWriteLock lock(path)"), std::string::npos);
    EXPECT_NE(fp.find("ConfigWriteLock lock(path)"), std::string::npos);
    EXPECT_NE(ad.find("ConfigWriteLock lock(trendPath())"), std::string::npos);
}
