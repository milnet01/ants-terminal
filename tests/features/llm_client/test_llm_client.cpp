// ANTS-1727 — LlmClient static-kernel feature test (no live network).
//
// INV-2  isEndpointAllowed scheme allowlist.
// INV-3  buildRequestBody scrubs secrets out of the serialised body.
// INV-4  accumulateCapped 10 MiB cap + truncated marker.
// INV-5  sseContentDelta SSE line parsing.
// INV-16 the three LLM modules are widget-free (source-grep).

#include "llmclient.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

// INV-2 — scheme allowlist.
TEST(LlmClient, INV2_EndpointAllowlist) {
    QString scheme;
    EXPECT_TRUE(LlmClient::isEndpointAllowed(
        QStringLiteral("https://api.openai.com/v1/chat/completions"), &scheme));
    EXPECT_EQ(scheme, QStringLiteral("https"));
    EXPECT_TRUE(LlmClient::isEndpointAllowed(
        QStringLiteral("http://127.0.0.1:11434/v1/chat/completions")));
    EXPECT_FALSE(LlmClient::isEndpointAllowed(QStringLiteral("file:///etc/passwd")));
    EXPECT_FALSE(LlmClient::isEndpointAllowed(QStringLiteral("gopher://host/x")));
    EXPECT_FALSE(LlmClient::isEndpointAllowed(QStringLiteral("api.openai.com/v1")));
    EXPECT_FALSE(LlmClient::isEndpointAllowed(QString()));
}

// INV-2 — isPlaintextRemote: http+remote yes; https or loopback no.
TEST(LlmClient, INV2_PlaintextRemote) {
    EXPECT_TRUE(LlmClient::isPlaintextRemote(QStringLiteral("http://example.com/v1")));
    EXPECT_FALSE(LlmClient::isPlaintextRemote(QStringLiteral("https://example.com/v1")));
    EXPECT_FALSE(LlmClient::isPlaintextRemote(QStringLiteral("http://localhost:1234/v1")));
    EXPECT_FALSE(LlmClient::isPlaintextRemote(QStringLiteral("http://127.0.0.1/v1")));
    // ANTS-1846 — the whole 127.0.0.0/8 loopback range + ::1, matching
    // isEndpointHostBlocked. 127.0.0.2 was wrongly treated as remote pre-fix.
    EXPECT_FALSE(LlmClient::isPlaintextRemote(QStringLiteral("http://127.0.0.2/v1")));
    EXPECT_FALSE(LlmClient::isPlaintextRemote(QStringLiteral("http://[::1]:1234/v1")));
}

// ANTS-1746 — SSRF host guard: private / link-local / metadata IP
// literals blocked; loopback, hostnames, and public IPs allowed.
TEST(LlmClient, Ants1746_EndpointHostBlocked) {
    // Blocked: cloud metadata + RFC-1918 + CGNAT + 0.0.0.0/8.
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://169.254.169.254/latest/meta-data/")));
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://10.0.0.5:11434/v1")));
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://192.168.1.50:1234/v1")));
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://172.16.3.4/v1")));
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://100.100.0.1/v1")));
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://0.0.0.0/v1")));
    // Blocked: IPv6 link-local + unique-local.
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://[fe80::1]/v1")));
    EXPECT_TRUE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://[fc00::1]/v1")));
    // Allowed: loopback (local dev LLM), hostnames, public IPs.
    EXPECT_FALSE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://127.0.0.1:11434/v1")));
    EXPECT_FALSE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://localhost:11434/v1")));
    EXPECT_FALSE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("http://[::1]:11434/v1")));
    EXPECT_FALSE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("https://api.openai.com/v1/chat/completions")));
    EXPECT_FALSE(LlmClient::isEndpointHostBlocked(
        QStringLiteral("https://8.8.8.8/v1")));
}

// INV-3 — secrets scrubbed out of the serialised request body.
TEST(LlmClient, INV3_BuildRequestBodyScrubsSecrets) {
    // A GitHub classic PAT shape (ghp_ + 36 alphanumerics) — a known
    // SecretRedact rule.
    // ghp_ + exactly 36 alphanumerics — the SecretRedact classic-PAT rule.
    const QString secret = QStringLiteral("ghp_") + QString(36, QLatin1Char('a'));
    LlmRequest req;
    req.model = QStringLiteral("gpt-4");
    req.systemPrompt = QStringLiteral("context with token ") + secret;
    req.userPrompt = QStringLiteral("also ") + secret;
    req.scrubSecrets = true;

    int redacted = 0;
    const QByteArray body = LlmClient::buildRequestBody(req, &redacted);
    EXPECT_GT(redacted, 0);
    EXPECT_FALSE(body.contains(secret.toUtf8())) << body.toStdString();
    EXPECT_TRUE(body.contains("[REDACTED:"));
}

// INV-3 — scrubSecrets=false leaves the prompt intact (redacted == 0).
TEST(LlmClient, INV3_NoScrubWhenDisabled) {
    // ghp_ + exactly 36 alphanumerics — the SecretRedact classic-PAT rule.
    const QString secret = QStringLiteral("ghp_") + QString(36, QLatin1Char('a'));
    LlmRequest req;
    req.systemPrompt = secret;
    req.scrubSecrets = false;
    int redacted = -1;
    const QByteArray body = LlmClient::buildRequestBody(req, &redacted);
    EXPECT_EQ(redacted, 0);
    EXPECT_TRUE(body.contains(secret.toUtf8()));
}

// INV-4 — accumulation cap flips truncated once; no further append.
TEST(LlmClient, INV4_AccumulateCapped) {
    QString acc;
    qint64 accBytes = 0;
    bool truncated = false;
    // Below cap: normal append, byte total advances.
    EXPECT_TRUE(LlmClient::accumulateCapped(acc, accBytes, truncated, QStringLiteral("hello")));
    EXPECT_FALSE(truncated);
    EXPECT_EQ(accBytes, 5);
    // Empty delta: no append, byte total unchanged.
    EXPECT_FALSE(LlmClient::accumulateCapped(acc, accBytes, truncated, QString()));
    EXPECT_EQ(accBytes, 5);

    // At the byte cap, the next append is refused + marker added once.
    accBytes = LlmClient::kMaxBytes;
    EXPECT_FALSE(LlmClient::accumulateCapped(acc, accBytes, truncated, QStringLiteral("more")));
    EXPECT_TRUE(truncated);
    EXPECT_TRUE(acc.contains(QStringLiteral("[response truncated]")));
    const int sizeAfterFirst = acc.size();
    // A second overflow attempt does not append a second marker.
    EXPECT_FALSE(LlmClient::accumulateCapped(acc, accBytes, truncated, QStringLiteral("again")));
    EXPECT_EQ(acc.size(), sizeAfterFirst);
}

// ANTS-1846 — the cap is byte-accurate: a multi-byte codepoint advances the
// byte total by its UTF-8 length (3), not its QString::size() unit count (1).
TEST(LlmClient, Ants1846_AccumulateBytesAreUtf8) {
    QString acc;
    qint64 accBytes = 0;
    bool truncated = false;
    const QString euro = QString::fromUtf8("\xE2\x82\xAC");  // € — 1 unit, 3 bytes
    ASSERT_EQ(euro.size(), 1);
    EXPECT_TRUE(LlmClient::accumulateCapped(acc, accBytes, truncated, euro));
    EXPECT_EQ(accBytes, 3) << "byte total must count UTF-8 bytes, not UTF-16 units";
}

// INV-5 — SSE line parsing table.
TEST(LlmClient, INV5_SseContentDelta) {
    EXPECT_EQ(LlmClient::sseContentDelta(
        QStringLiteral("data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}")),
        QStringLiteral("hi"));
    // No space after "data:".
    EXPECT_EQ(LlmClient::sseContentDelta(
        QStringLiteral("data:{\"choices\":[{\"delta\":{\"content\":\"x\"}}]}")),
        QStringLiteral("x"));
    EXPECT_TRUE(LlmClient::sseContentDelta(QStringLiteral("data: [DONE]")).isEmpty());
    EXPECT_TRUE(LlmClient::sseContentDelta(QStringLiteral(": ping")).isEmpty());
    EXPECT_TRUE(LlmClient::sseContentDelta(
        QStringLiteral("data: {\"choices\":[]}")).isEmpty());
    EXPECT_TRUE(LlmClient::sseContentDelta(QStringLiteral("garbage")).isEmpty());
}

// INV-16 — the three LLM modules include no Qt Widgets header.
// (llmdispatcher entries added with that module in ANTS-1727 step 3.)
TEST(LlmClient, INV16_WidgetFree) {
    const char *paths[] = {
        SRC_LLMCLIENT_H_PATH,     SRC_LLMCLIENT_CPP_PATH,
        SRC_LLMDISPATCHER_H_PATH, SRC_LLMDISPATCHER_CPP_PATH,
        SRC_BRIEFDISPATCH_H_PATH, SRC_BRIEFDISPATCH_CPP_PATH,
    };
    for (const char *p : paths) {
        const std::string src = ants_test::slurpFile(p);
        ASSERT_FALSE(src.empty()) << "could not read " << p;
        EXPECT_EQ(src.find("<QtWidgets"), std::string::npos) << p;
        EXPECT_EQ(src.find("#include <QWidget"), std::string::npos) << p;
        EXPECT_EQ(src.find("#include <QDialog"), std::string::npos) << p;
    }
}
