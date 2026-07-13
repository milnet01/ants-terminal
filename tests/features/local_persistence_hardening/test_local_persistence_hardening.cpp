// Feature-conformance test for spec.md — the indie-review #6
// local-persistence write-safety bundle (ANTS-1821/1822/1823/1824/1836).
//
// PD-* exercise ensurePrivateDir directly (secureio.h, header-only).
// ML-* exercise SessionMemoryEngine::mutateLocked (ants_core_lib).
// WI-* grep the wired call sites for the five fixes.

#include "../../_support/expect.h"
#include "secureio.h"
#include "sessionmemoryengine.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTemporaryDir>

#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_SECUREIO_H_PATH
#error "SRC_SECUREIO_H_PATH compile definition required"
#endif
#ifndef SRC_SESSIONMANAGER_CPP_PATH
#error "SRC_SESSIONMANAGER_CPP_PATH compile definition required"
#endif
#ifndef SRC_SESSIONMEMORYENGINE_CPP_PATH
#error "SRC_SESSIONMEMORYENGINE_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITFPLEDGER_CPP_PATH
#error "SRC_AUDITFPLEDGER_CPP_PATH compile definition required"
#endif
#ifndef SRC_AUDITAUTOFIX_CPP_PATH
#error "SRC_AUDITAUTOFIX_CPP_PATH compile definition required"
#endif
#ifndef SRC_TESTAUDITDIALOG_CPP_PATH
#error "SRC_TESTAUDITDIALOG_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

namespace SME = SessionMemoryEngine;


int dirMode(const QString &path) {
    struct stat st{};
    if (::lstat(path.toLocal8Bit().constData(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return -1;
    return st.st_mode & 0777;
}

// ---- ensurePrivateDir (ANTS-1821) ---------------------------------

void testEnsurePrivateDir() {
    // INV-1 — missing dir (incl. a missing parent) born 0700.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString leaf = tmp.path() + QStringLiteral("/ants-x/sessions");
        expect(ensurePrivateDir(leaf), "PD-1 creates nested dir");
        expect(QDir(leaf).exists(), "PD-1 dir exists post-call");
        expect(dirMode(leaf) == 0700,
               (std::string("PD-1 leaf mode 0700, got 0")
                + std::to_string(dirMode(leaf))).c_str());
        expect(dirMode(tmp.path() + QStringLiteral("/ants-x")) == 0700,
               "PD-1 created intermediate is also 0700");
    }

    // INV-2 — idempotent; second call keeps an existing child.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString leaf = tmp.path() + QStringLiteral("/d");
        expect(ensurePrivateDir(leaf), "PD-2 first call creates");
        QFile marker(leaf + QStringLiteral("/marker"));
        expect(marker.open(QIODevice::WriteOnly), "PD-2 marker writable");
        marker.close();
        expect(ensurePrivateDir(leaf), "PD-2 second call ok");
        expect(QFile::exists(leaf + QStringLiteral("/marker")),
               "PD-2 idempotent — marker survives");
    }

    // INV-3 — tightens a pre-existing 0755 leaf to 0700.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString leaf = tmp.path() + QStringLiteral("/loose");
        ASSERT_TRUE(QDir().mkdir(leaf));
        ::chmod(leaf.toLocal8Bit().constData(), 0755);
        expect(ensurePrivateDir(leaf), "PD-3 accepts existing leaf");
        expect(dirMode(leaf) == 0700,
               (std::string("PD-3 0755 leaf tightened to 0700, got 0")
                + std::to_string(dirMode(leaf))).c_str());
    }

    // INV-4 — symlink leaf rejected, not followed.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString real = tmp.path() + QStringLiteral("/real");
        ASSERT_TRUE(QDir().mkdir(real));
        const QString link = tmp.path() + QStringLiteral("/link");
        ASSERT_EQ(0, ::symlink(real.toLocal8Bit().constData(),
                               link.toLocal8Bit().constData()));
        expect(!ensurePrivateDir(link), "PD-4 symlink leaf rejected");
    }
}

// ---- mutateLocked (ANTS-1823) -------------------------------------

void testMutateLocked() {
    // INV-5 — read-only mutator writes nothing.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString path = SME::storePathFor("/cwd", tmp.path());
        const auto r = SME::mutateLocked(path,
            [](QJsonObject &) { return false; });
        expect(r.ok, "ML-5 read-only mutate ok");
        expect(!QFile::exists(path), "ML-5 no store file written");
    }

    // INV-7 — write then re-load round-trips.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString path = SME::storePathFor("/cwd", tmp.path());
        const auto r = SME::mutateLocked(path, [](QJsonObject &s) {
            s[QStringLiteral("a")] = QStringLiteral("1");
            return true;
        });
        expect(r.ok, "ML-7 write ok");
        expect(QFile::exists(path), "ML-7 store file exists");
        const QJsonObject back = SME::loadStore(path);
        expect(back.value(QStringLiteral("a")).toString()
                   == QStringLiteral("1"),
               "ML-7 round-trips the written key");
    }

    // INV-6 — total-bytes cap → cap_exceeded, nothing written.
    {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        const QString path = SME::storePathFor("/cwd", tmp.path());
        const QString big(SME::kMaxStoreBytes + 1024, QChar('x'));
        const auto r = SME::mutateLocked(path, [&](QJsonObject &s) {
            s[QStringLiteral("big")] = big;
            return true;
        });
        expect(!r.ok, "ML-6 over-cap mutate refused");
        expect(r.code == QStringLiteral("cap_exceeded"),
               (std::string("ML-6 code cap_exceeded, got ")
                + r.code.toStdString()).c_str());
        expect(!QFile::exists(path), "ML-6 nothing written past the cap");
    }
}

// ---- Wiring greps (INV-8) -----------------------------------------

void testWiring() {
    const std::string secio = ants_test::slurpFile(SRC_SECUREIO_H_PATH);
    const std::string sm    = ants_test::slurpFile(SRC_SESSIONMANAGER_CPP_PATH);
    const std::string sme   = ants_test::slurpFile(SRC_SESSIONMEMORYENGINE_CPP_PATH);
    const std::string fp    = ants_test::slurpFile(SRC_AUDITFPLEDGER_CPP_PATH);
    const std::string af    = ants_test::slurpFile(SRC_AUDITAUTOFIX_CPP_PATH);
    const std::string ta    = ants_test::slurpFile(SRC_TESTAUDITDIALOG_CPP_PATH);
    const std::string rc    = ants_test::slurpFile(SRC_RC_CPP);

    // ANTS-1821 — helper defined; sessionDir + saveStore use it; the
    // mkpath-then-chmod idiom is gone from sessionDir.
    expect(secio.find("ensurePrivateDir") != std::string::npos,
           "WI-1821a ensurePrivateDir defined in secureio.h");
    expect(sm.find("ensurePrivateDir(dir)") != std::string::npos,
           "WI-1821b sessionDir uses ensurePrivateDir");
    expect(sm.find("QDir().mkpath(dir)") == std::string::npos,
           "WI-1821c sessionDir no longer mkpath-then-chmod");
    expect(sme.find("ensurePrivateDir(") != std::string::npos,
           "WI-1821d saveStore uses ensurePrivateDir");

    // ANTS-1822 — cleanup glob scoped to the two owned temp names.
    expect(sm.find("session_*.dat.tmp") != std::string::npos
               && sm.find("tab_order.txt.tmp") != std::string::npos,
           "WI-1822a cleanupOldSessions globs the two owned temps");
    expect(sm.find("entryInfoList({\"*.tmp\"}") == std::string::npos,
           "WI-1822b bare *.tmp glob replaced");

    // ANTS-1823 — locked RMW primitive + workflow_state routes through it.
    expect(sme.find("mutateLocked") != std::string::npos,
           "WI-1823a mutateLocked defined in the engine");
    expect(rc.find("mutateLocked(storePath") != std::string::npos,
           "WI-1823b workflow_state set uses mutateLocked");

    // ANTS-1824 — single record+newline buffer in both appenders.
    expect(fp.find("Compact) + '\\n'") != std::string::npos,
           "WI-1824a fp ledger single-buffer append");
    expect(fp.find("f.write(\"\\n\")") == std::string::npos,
           "WI-1824b fp ledger standalone newline write removed");
    expect(af.find("Compact) + '\\n'") != std::string::npos,
           "WI-1824c autofix log single-buffer append");
    expect(af.find("f.write(\"\\n\")") == std::string::npos,
           "WI-1824d autofix log standalone newline write removed");

    // ANTS-1836 — writeReport atomic + 0600.
    expect(ta.find("QSaveFile") != std::string::npos,
           "WI-1836a writeReport uses QSaveFile");
    expect(ta.find("setOwnerOnlyPerms") != std::string::npos,
           "WI-1836b writeReport sets 0600");
}

int runMain() {
    expect_reset();
    testEnsurePrivateDir();
    testMutateLocked();
    testWiring();
    return expect_finish();
}

}  // namespace

TEST(LocalPersistenceHardening, Main) { ASSERT_EQ(0, runMain()); }
