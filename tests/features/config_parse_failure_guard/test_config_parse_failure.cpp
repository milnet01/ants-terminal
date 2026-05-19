// Feature-conformance test for spec.md — asserts that Config::load()
// distinguishes fresh/ok/corrupt, latches m_loadFailed on corrupt
// input, rotates the corrupt file aside, and suppresses save() so a
// setter-triggered write can't destroy the user's broken-but-
// hand-fixable config.json.
//
// The test uses a sandboxed XDG_CONFIG_HOME so it cannot touch the
// real user config. Each scenario plants a specific file state,
// constructs a Config, and asserts observable state + filesystem.
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "../../_support/expect.h"
#include "config.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <gtest/gtest.h>
ANTS_TEST_SCOPE();

namespace {



// RAII guard for XDG_CONFIG_HOME — captures the prior value (or the
// "unset" state) and restores it in the destructor so each scenario's
// sandbox doesn't leak to the next test in the same bundle binary
// (test_core runs config_parse_failure_guard, config_reload_loop_safety,
// debuglog_perms, and session_persistence_default sequentially; without
// this guard, the last sandbox's XDG_CONFIG_HOME stays pointing at a
// deleted QTemporaryDir for every subsequent test).
struct XdgConfigHomeGuard {
    QByteArray prior;
    bool hadPrior = false;
    XdgConfigHomeGuard() {
        hadPrior = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        if (hadPrior) prior = qgetenv("XDG_CONFIG_HOME");
    }
    void set(const QByteArray &v) { qputenv("XDG_CONFIG_HOME", v); }
    ~XdgConfigHomeGuard() {
        if (hadPrior) qputenv("XDG_CONFIG_HOME", prior);
        else qunsetenv("XDG_CONFIG_HOME");
    }
};

// Plant a specific `config.json` inside a sandboxed XDG_CONFIG_HOME
// and return the directory that contains it. `content == nullptr`
// means "don't create the file" (fresh-run scenario).
//
// Qt's QStandardPaths on Linux honors $XDG_CONFIG_HOME at call-time
// (no caching), so qputenv here is the whole sandbox. Do NOT use
// QStandardPaths::setTestModeEnabled — it routes to a Qt-test-specific
// path that ignores XDG and has collided with real user configs in
// practice. Caller owns the XdgConfigHomeGuard so the env restore
// happens after the QTemporaryDir is consumed.
QString setupSandbox(QTemporaryDir &tmp, XdgConfigHomeGuard &guard,
                     const char *content) {
    guard.set(tmp.path().toLocal8Bit());

    const QString antsDir = tmp.path() + "/ants-terminal";
    QDir().mkpath(antsDir);

    if (content) {
        QFile f(antsDir + "/config.json");
        if (!f.open(QIODevice::WriteOnly)) return QString();
        f.write(content);
        f.close();
    }

    return antsDir;
}

void testFreshRun() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "freshRun: sandbox setup"); return; }
    XdgConfigHomeGuard envGuard;
    QString antsDir = setupSandbox(tmp, envGuard, nullptr);

    Config cfg;

    expect(!cfg.loadFailed(), "freshRun: loadFailed() == false");
    expect(cfg.loadFailureBackupPath().isEmpty(),
           "freshRun: no backup path");

    // Setter triggers save(); config.json should now exist.
    cfg.setFontSize(13);
    expect(QFile::exists(antsDir + "/config.json"),
           "freshRun: setter save() creates config.json");
}

void testValidConfig() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "validConfig: sandbox setup"); return; }
    XdgConfigHomeGuard envGuard;
    setupSandbox(tmp, envGuard, R"({"theme":"Dark","font_size":14})");

    Config cfg;

    expect(!cfg.loadFailed(), "validConfig: loadFailed() == false");
    expect(cfg.fontSize() == 14,
           "validConfig: parsed font_size honored",
           QString("got %1").arg(cfg.fontSize()).toUtf8().constData());
}

void testCorruptConfig() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "corruptConfig: sandbox setup"); return; }
    XdgConfigHomeGuard envGuard;
    const char *corrupt = "{\"theme\":\"Dark\",\"font_size\":14,"; // truncated
    QString antsDir = setupSandbox(tmp, envGuard, corrupt);
    const QString cfgPath = antsDir + "/config.json";

    // Capture original bytes — we'll assert they survive unchanged.
    QFile before(cfgPath);
    if (!before.open(QIODevice::ReadOnly)) {
        expect(false, "corruptConfig: could not open planted corrupt file");
        return;
    }
    const QByteArray originalBytes = before.readAll();
    before.close();

    Config cfg;

    expect(cfg.loadFailed(), "corruptConfig: loadFailed() == true");
    expect(!cfg.loadFailureBackupPath().isEmpty(),
           "corruptConfig: backup path reported");
    expect(QFile::exists(cfg.loadFailureBackupPath()),
           "corruptConfig: backup file actually written");

    // Verify backup content matches original corrupt bytes
    QFile bak(cfg.loadFailureBackupPath());
    if (bak.open(QIODevice::ReadOnly)) {
        expect(bak.readAll() == originalBytes,
               "corruptConfig: backup content matches corrupt original");
        bak.close();
    } else {
        expect(false, "corruptConfig: could not open backup for verification");
    }

    // The critical assertion: a setter must NOT overwrite the corrupt
    // config.json. Before 0.7.12, setFontSize would trigger save() which
    // would write fresh defaults and destroy the user's prior bytes.
    cfg.setFontSize(99);

    QFile after(cfgPath);
    if (!after.open(QIODevice::ReadOnly)) {
        expect(false,
               "corruptConfig: config.json disappeared after setter — "
               "save() must leave it alone, not delete it");
        return;
    }
    const QByteArray afterBytes = after.readAll();
    after.close();

    expect(afterBytes == originalBytes,
           "corruptConfig: setter's save() did NOT overwrite corrupt file");
}

void testEmptyStringParsesAsNull() {
    // An empty file parses as null doc → not an object → loadFailed
    // path. Common outcome of a failed write mid-fsync.
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "emptyFile: sandbox setup"); return; }
    XdgConfigHomeGuard envGuard;
    setupSandbox(tmp, envGuard, "");

    Config cfg;
    expect(cfg.loadFailed(), "emptyFile: empty file → loadFailed == true");
}

void testNonObjectJson() {
    // Valid JSON but not an object — e.g. array. Same protection
    // applies: don't clobber whatever the user had.
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "nonObject: sandbox setup"); return; }
    XdgConfigHomeGuard envGuard;
    setupSandbox(tmp, envGuard, "[1,2,3]");

    Config cfg;
    expect(cfg.loadFailed(), "nonObject: JSON array → loadFailed == true");
}

// Invariant 6 — `.corrupt-<ms>` backups are capped at the newest 5.
// Repeatedly opening Ants with a broken config would otherwise
// accumulate one backup per launch forever. Plant 8 stale backups
// from an earlier-timestamp run, then trip one more parse-failure —
// afterwards only the 5 newest (4 old + 1 fresh) may remain. Implemented
// inside secureio.h::rotateCorruptFileAside so Config + ClaudeAllowlist +
// SettingsDialog all benefit from the same cap.
void testRetentionCap() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "retentionCap: sandbox setup"); return; }
    XdgConfigHomeGuard envGuard;
    QString antsDir = setupSandbox(tmp, envGuard, "{bad");
    const QString cfgPath = antsDir + "/config.json";

    // Plant 8 synthetic `.corrupt-*` siblings with decreasing (older)
    // mtimes. The retention logic uses QDir::Time (mtime) to rank them,
    // so we MUST set the file mtime explicitly (QFile::open alone would
    // give all 8 a near-identical "now" mtime and the "newest 5"
    // selection would be effectively random).
    const QDateTime now = QDateTime::currentDateTime();
    for (int i = 0; i < 8; ++i) {
        const qint64 stampMs = now.toMSecsSinceEpoch() - (i + 1) * 60000;
        const QString old = cfgPath + QStringLiteral(".corrupt-")
                          + QString::number(stampMs);
        QFile f(old);
        if (!f.open(QIODevice::WriteOnly)) continue;
        f.write("stale");
        f.setFileTime(QDateTime::fromMSecsSinceEpoch(stampMs),
                      QFileDevice::FileModificationTime);
        f.close();
    }

    auto countCorrupt = [&]() {
        QDir d(antsDir);
        return d.entryList({QStringLiteral("config.json.corrupt-*")},
                           QDir::Files).size();
    };

    expect(countCorrupt() == 8,
           "retentionCap: planted 8 stale .corrupt-* siblings");

    // Trigger a fresh rotateCorruptFileAside via Config::load(). The
    // prune loop should remove everything past the newest 5.
    Config cfg;
    expect(cfg.loadFailed(), "retentionCap: fresh parse failure latched");
    expect(!cfg.loadFailureBackupPath().isEmpty(),
           "retentionCap: fresh .corrupt-<ms> backup written");

    // Expected after prune: newest 5 = {fresh backup, 4 newest stale}.
    const auto remaining = countCorrupt();
    expect(remaining == 5,
           "retentionCap: pruned to newest 5",
           QString("got %1").arg(remaining).toUtf8().constData());
}

}  // namespace

static int runMain() {
    expect_reset();
    // ANTS-1217: bundle_main creates the QCoreApplication

    testFreshRun();
    testValidConfig();
    testCorruptConfig();
    testEmptyStringParsesAsNull();
    testNonObjectJson();
    testRetentionCap();

    return expect_finish();
}

TEST(ConfigParseFailureGuard, Main) {
    ASSERT_EQ(0, runMain());
}
