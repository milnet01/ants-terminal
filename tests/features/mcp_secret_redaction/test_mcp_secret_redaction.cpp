// Secrets hidden from the terminal-reading MCP verbs — see spec.md. ANTS-5169.

#include "remotecontrol.h"
#include "claudeintegration.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#ifndef ANTS_MAINWINDOW_SOURCES
#  error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#  error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#  error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

// A GitHub classic token shape: ghp_ + 36 alphanumerics.
QString token() { return QStringLiteral("ghp_") + QString(36, QLatin1Char('a')); }

std::string between(const std::string &s, const std::string &from,
                    const std::string &to) {
    const std::size_t a = s.find(from);
    if (a == std::string::npos) return {};
    const std::size_t b = s.find(to, a + from.size());
    if (b == std::string::npos) return {};
    return s.substr(a, b - a);
}

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// True when `first` occurs in `s` and before any `second`.
bool before(const std::string &s, const std::string &first,
            const std::string &second) {
    const std::size_t a = s.find(first);
    const std::size_t b = s.find(second);
    return a != std::string::npos && b != std::string::npos && a < b;
}

std::string rcSource() {
    return ants_test::stripComments(ants_test::slurpRemoteControl());
}

std::string verbBody(const std::string &name) {
    return between(rcSource(), "QJsonDocument RemoteControl::" + name + "(",
                   "\nQJsonDocument RemoteControl::");
}

std::string scrollbackProvider() {
    const std::string mw = ants_test::stripComments(ants_test::slurpMainWindow());
    return between(mw, "registerToolProvider(\"get_scrollback\"",
                   "registerToolProvider(");
}

constexpr const char *kSwitch = "Config().claudeMcpRedactSecrets()";

}  // namespace

// INV-1
TEST(McpSecretRedaction, Inv1RedactsWhenEnabled) {
    const QString raw = QStringLiteral("export GH=") + token() + QStringLiteral("\nls\n");
    const auto on = RemoteControl::redactForClaude(raw, true);
    EXPECT_FALSE(on.text.contains(token())) << on.text.toStdString();
    EXPECT_TRUE(on.text.contains(QStringLiteral("[REDACTED:github_pat]")));
    EXPECT_EQ(on.redacted, 1);

    const auto off = RemoteControl::redactForClaude(raw, false);
    EXPECT_EQ(off.text, raw);
    EXPECT_EQ(off.redacted, 0);
}

// INV-2
TEST(McpSecretRedaction, Inv2SwitchDefaultsOn) {
    const std::string cfg = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/config.cpp");
    EXPECT_TRUE(has(cfg, "value(\"claude.mcp_redact_secrets\").toBool(true)"));
}

// INV-3 + INV-4
TEST(McpSecretRedaction, Inv3EveryVerbRedacts) {
    const std::string getText = verbBody("cmdGetText");
    ASSERT_FALSE(getText.empty());
    EXPECT_TRUE(has(getText, kSwitch));
    EXPECT_TRUE(before(getText, "redactForClaude(", "trimScrollbackForGetText("));
    EXPECT_TRUE(has(getText, "out[\"redacted\"]"));

    for (const char *verb : {"cmdRecentErrors", "cmdLastSelection"}) {
        const std::string body = verbBody(verb);
        ASSERT_FALSE(body.empty()) << verb;
        EXPECT_TRUE(has(body, kSwitch)) << verb;
        EXPECT_TRUE(has(body, "redactForClaude(")) << verb;
        EXPECT_TRUE(has(body, "\"redacted\"")) << verb;
    }

    const std::string sb = scrollbackProvider();
    ASSERT_FALSE(sb.empty());
    EXPECT_TRUE(has(sb, kSwitch));
    EXPECT_TRUE(before(sb, "redactForClaude(", "trimScrollbackForGetText("));
    EXPECT_TRUE(has(sb, "<redacted %1 secrets>"));
    EXPECT_TRUE(has(sb, "env[QStringLiteral(\"redacted\")]"));
}

// ANTS-5098 — last_selection had no size cap. It now goes through get_text's
// trim, after redaction, so the cap bounds what is sent and never cuts a
// secret in half; it takes max_bytes and reports truncated as get_text does.
TEST(McpSecretRedaction, Ants5098LastSelectionTrimsAfterRedacting) {
    const std::string body = verbBody("cmdLastSelection");
    ASSERT_FALSE(body.empty());
    EXPECT_TRUE(before(body, "redactForClaude(", "trimScrollbackForGetText("));
    EXPECT_TRUE(has(body, "\"max_bytes\""));
    EXPECT_TRUE(has(body, "\"truncated\""));
}

// INV-5
TEST(McpSecretRedaction, Inv5SecretFileNames) {
    for (const char *p : {".env", "/proj/.env.local", "/etc/ssl/server.pem",
                          "/home/u/.ssh/id_ed25519", "id_rsa.pub"})
        EXPECT_TRUE(ClaudeIntegration::isSecretFileName(QString::fromUtf8(p))) << p;
    for (const char *p : {"README.md", "/proj/src/environment.ts", "/proj/id.txt",
                          "/srv/.env/notes.txt", "/proj/pem.cpp", ""})
        EXPECT_FALSE(ClaudeIntegration::isSecretFileName(QString::fromUtf8(p))) << p;
}

// INV-6
TEST(McpSecretRedaction, Inv6ReadOfSecretFileWarns) {
    const std::string ci = ants_test::stripComments(
        ants_test::slurpFile(ANTS_SOURCE_DIR "/src/claudeintegration.cpp"));
    const std::string body = between(ci, "void ClaudeIntegration::updateChangedFiles(",
                                     "\n}\n");
    ASSERT_FALSE(body.empty());
    EXPECT_TRUE(before(body, "emit sensitiveFileRead(", "m_changedFiles.contains("));
    EXPECT_TRUE(has(body, "isSecretFileName("));

    const std::string sw = ants_test::slurpFile(ANTS_SOURCE_DIR "/src/claudestatuswidgets.cpp");
    EXPECT_TRUE(has(sw, "&ClaudeIntegration::sensitiveFileRead"));
}
