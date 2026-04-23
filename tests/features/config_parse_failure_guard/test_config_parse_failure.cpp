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

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const char *detail = "") {
    std::fprintf(stderr, "[%-60s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 (detail && *detail) ? " — " : "",
                 detail ? detail : "");
    if (!ok) ++g_failures;
}

// Plant a specific `config.json` inside a sandboxed XDG_CONFIG_HOME
// and return the directory that contains it. `content == nullptr`
// means "don't create the file" (fresh-run scenario).
//
// Qt's QStandardPaths on Linux honors $XDG_CONFIG_HOME at call-time
// (no caching), so qputenv here is the whole sandbox. Do NOT use
// QStandardPaths::setTestModeEnabled — it routes to a Qt-test-specific
// path that ignores XDG and has collided with real user configs in
// practice.
QString setupSandbox(QTemporaryDir &tmp, const char *content) {
    qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());

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
    QString antsDir = setupSandbox(tmp, nullptr);

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
    setupSandbox(tmp, R"({"theme":"Dark","font_size":14})");

    Config cfg;

    expect(!cfg.loadFailed(), "validConfig: loadFailed() == false");
    expect(cfg.fontSize() == 14,
           "validConfig: parsed font_size honored",
           QString("got %1").arg(cfg.fontSize()).toUtf8().constData());
}

void testCorruptConfig() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "corruptConfig: sandbox setup"); return; }
    const char *corrupt = "{\"theme\":\"Dark\",\"font_size\":14,"; // truncated
    QString antsDir = setupSandbox(tmp, corrupt);
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
    setupSandbox(tmp, "");

    Config cfg;
    expect(cfg.loadFailed(), "emptyFile: empty file → loadFailed == true");
}

void testNonObjectJson() {
    // Valid JSON but not an object — e.g. array. Same protection
    // applies: don't clobber whatever the user had.
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "nonObject: sandbox setup"); return; }
    setupSandbox(tmp, "[1,2,3]");

    Config cfg;
    expect(cfg.loadFailed(), "nonObject: JSON array → loadFailed == true");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    testFreshRun();
    testValidConfig();
    testCorruptConfig();
    testEmptyStringParsesAsNull();
    testNonObjectJson();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d assertion(s) failed.\n", g_failures);
        return 1;
    }
    return 0;
}
