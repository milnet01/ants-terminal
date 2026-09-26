// mcpd_verify_trust_wiring — MC-5. Contract: spec.md here, extending the
// MC-numbering `tests/features/verify_trust_gate/spec.md` reserves.
//
// Why this exists: src/mcpdmain.cpp:91 constructs ants-mcpd's RemoteControl
// (`RemoteControl rc(nullptr, nullptr, &roots);`) and never calls
// setVerifyTrustClient on it, so VerifyEngine::loadGateConfig's null-client
// back-compat branch (src/verifyengine.cpp:426-430 — "honour it
// unconditionally") fires on every verify_changes call ants-mcpd serves,
// for every project. An untrusted repo's bespoke `.ants/verify.json` build
// command runs with no trust prompt and no auto-detect fallback, on the one
// host (Claude Code talks to ants-mcpd, not the GUI terminal's own MCP path)
// that actually matters for a cloned-repo attack.

#include "../standalone_mcp_server/mcpd_session.h"
#include "../../_support/xdg_guard.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#ifndef ANTS_MCPD_BIN
#error "ANTS_MCPD_BIN compile definition required"
#endif

using ants_test::McpdSession;

namespace {

// A socket path with no listener — no case here forwards to a terminal.
QString deadSocket(const QTemporaryDir &tmp) {
    return tmp.filePath(QStringLiteral("no-terminal.sock"));
}

}  // namespace

// MC-5 — an untrusted bespoke .ants/verify.json build command must not run
// through ants-mcpd, and the response must say it fell back.
TEST(McpdVerifyTrustWiring, UntrustedBespokeConfigDoesNotRun) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // A fresh, empty trust store and no autotrust bypass. HOME / XDG_CONFIG_HOME
    // point at a temp directory so the real ~/.config/ants-terminal/
    // verify-trust.json is never read or written by this test, and whatever
    // trust file VerifyTrust::FilePersistedTrustClient's default ctor would
    // resolve starts out absent (empty trust set) inside that sandbox.
    ants_test::XdgGuard env;
    const QString fakeHome = tmp.filePath(QStringLiteral("home"));
    ASSERT_TRUE(QDir().mkpath(fakeHome));
    env.setEnv("HOME", fakeHome.toUtf8());
    env.setEnv("XDG_CONFIG_HOME",
               (fakeHome + QStringLiteral("/.config")).toUtf8());
    env.unsetEnv("ANTS_VERIFY_TRUST_AUTOTRUST");

    // The fixture project has no CMakeLists.txt / package.json / Cargo.toml /
    // pyproject.toml, so VerifyEngine::autoDetect finds nothing to run.
    // That makes the marker file a clean signal: it can only appear if the
    // bespoke .ants/verify.json command actually ran.
    const QString raw = tmp.filePath(QStringLiteral("proj"));
    ASSERT_TRUE(QDir().mkpath(raw + QStringLiteral("/.ants")));
    const QString root = QFileInfo(raw).canonicalFilePath();
    ASSERT_FALSE(root.isEmpty());

    const QString marker = tmp.filePath(QStringLiteral("marker-created"));
    ASSERT_FALSE(QFileInfo::exists(marker));

    QFile cfg(root + QStringLiteral("/.ants/verify.json"));
    ASSERT_TRUE(cfg.open(QIODevice::WriteOnly));
    const QByteArray cfgJson =
        QByteArray("{\"build\":{\"command\":\"touch '") + marker.toUtf8()
        + QByteArray("'\",\"format\":\"plain\"}}");
    ASSERT_GT(cfg.write(cfgJson), 0);
    cfg.close();

    McpdSession mcpd(root, deadSocket(tmp));
    ASSERT_TRUE(mcpd.started());

    const QJsonObject resp = mcpd.call(QStringLiteral("verify_changes"),
        QJsonObject{{"caller_cwd", root}});
    ASSERT_FALSE(resp.value(QStringLiteral("test_timeout")).toBool())
        << "ants-mcpd never replied: "
        << QJsonDocument(resp).toJson().toStdString();

    // The defect: with no VerifyTrust client wired, loadGateConfig's
    // null-client back-compat branch honours the bespoke config
    // unconditionally, so the marker gets created and the response reports
    // the bespoke config as the (trusted) source.
    EXPECT_FALSE(QFileInfo::exists(marker))
        << "the untrusted bespoke build command ran through ants-mcpd - "
           "response: " << QJsonDocument(resp).toJson().toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("verify_untrusted")).toBool())
        << "expected verify_untrusted:true for a never-trusted bespoke "
           "config - response: " << QJsonDocument(resp).toJson().toStdString();
    EXPECT_NE(resp.value(QStringLiteral("config_source")).toString(),
              QStringLiteral(".ants/verify.json"))
        << "config_source reports the bespoke config as trusted - "
           "response: " << QJsonDocument(resp).toJson().toStdString();
}
