// ANTS-1727 — LlmClient static-kernel feature test (no live network).
//
// INV-2  isEndpointAllowed scheme allowlist.
// INV-3  buildRequestBody scrubs secrets out of the serialised body.
// INV-4  accumulateCapped 10 MiB cap + truncated marker.
// INV-5  sseContentDelta SSE line parsing.
// INV-8  endpointEgressError shared 4-gate validator + auditdialog wiring.
// INV-16 the three LLM modules are widget-free (source-grep).

#include "llmclient.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QObject>
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

// ANTS-2019 — a rapid re-send cancels the prior deferred error. Two rejected
// endpoints back-to-back must emit finished() exactly once (the latest), not
// twice. Pre-fix both QTimer::singleShot(0) callbacks fired.
TEST(LlmClient, Ants2019_ReSendCancelsStaleDeferredError) {
    LlmClient client;
    int finishedCount = 0;
    QObject::connect(&client, &LlmClient::finished, &client,
                     [&](const LlmResult &) { ++finishedCount; });

    LlmRequest bad1;
    bad1.endpoint = QStringLiteral("file:///etc/passwd");   // scheme rejected
    LlmRequest bad2;
    bad2.endpoint = QStringLiteral("gopher://host/x");      // scheme rejected
    client.send(bad1);   // schedules deferred error #1
    client.send(bad2);   // must cancel #1, schedule #2

    // Drain the event loop so both singleShot(0) callbacks would run.
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    EXPECT_EQ(finishedCount, 1)
        << "only the latest rejected send must emit finished()";
}

// ANTS-2108 — send() is the single egress chokepoint, so the cleartext-remote
// Bearer guard lives here as a backstop: the auditdialog batch path and the v2
// review dialogs (coldeyesdialog → ReviewDialogBase → send) never pre-checked
// isPlaintextRemote, so a key could ship in cleartext. send() now refuses
// before touching the network (gated on a non-empty key, matching the
// aidialog/auditdialog single-path guards; localhost stays exempt).
TEST(LlmClient, Ants2108_SendRefusesCleartextRemoteBearer) {
    LlmClient client;
    LlmResult captured;
    int n = 0;
    QObject::connect(&client, &LlmClient::finished, &client,
                     [&](const LlmResult &r) { captured = r; ++n; });

    LlmRequest req;
    req.endpoint = QStringLiteral("http://example.com/v1/chat/completions");
    req.apiKey = QStringLiteral("sk-secret");
    client.send(req);

    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    EXPECT_EQ(n, 1);
    EXPECT_FALSE(captured.ok);
    EXPECT_TRUE(captured.error.contains(QStringLiteral("cleartext"), Qt::CaseInsensitive))
        << captured.error.toStdString();
    EXPECT_FALSE(client.busy()) << "refusal must not open a network reply";
}

// ANTS-2109 H1 — embedded URL userinfo (user:pass@host) would travel out as a
// Basic-auth header derived from the URL, unscrubbed and in addition to the
// Bearer key. send() refuses it before posting, scheme-agnostic (https leaks
// the creds too).
TEST(LlmClient, Ants2109_SendRefusesUrlUserinfo) {
    for (const QString &ep : {
             QStringLiteral("https://user:secret@api.openai.com/v1/chat/completions"),
             QStringLiteral("http://user:secret@example.com/v1"),
         }) {
        LlmClient client;
        LlmResult captured;
        int n = 0;
        QObject::connect(&client, &LlmClient::finished, &client,
                         [&](const LlmResult &r) { captured = r; ++n; });

        LlmRequest req;
        req.endpoint = ep;
        req.apiKey = QStringLiteral("sk-secret");
        client.send(req);

        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        EXPECT_EQ(n, 1) << ep.toStdString();
        EXPECT_FALSE(captured.ok) << ep.toStdString();
        EXPECT_TRUE(captured.error.contains(QStringLiteral("credential"), Qt::CaseInsensitive))
            << captured.error.toStdString();
        EXPECT_FALSE(client.busy()) << ep.toStdString();
    }
}

// ANTS-2121 INV-8 — endpointEgressError is the single shared egress validator.
// Both LlmClient::send and the AuditDialog AI-triage POSTs run every request
// through it, so the scheme allowlist, URL-userinfo refusal, SSRF host-block,
// and cleartext-remote Bearer refusal are enforced identically. Empty == pass;
// the returned reason carries no channel prefix (callers add their own).
TEST(LlmClient, Ants2121_EndpointEgressError) {
    const QString key = QStringLiteral("sk-secret");
    // Passes: https public endpoint with a key; loopback http keyed (exempt).
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("https://api.openai.com/v1/chat/completions"), key).isEmpty());
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("http://127.0.0.1:11434/v1/chat/completions"), key).isEmpty());
    // Bad scheme.
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("file:///etc/passwd"), key)
        .contains(QStringLiteral("http"), Qt::CaseInsensitive));
    // URL userinfo (ANTS-2109 H1).
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("https://user:pass@api.openai.com/v1"), key)
        .contains(QStringLiteral("credential"), Qt::CaseInsensitive));
    // SSRF metadata IP literal (ANTS-1746).
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("http://169.254.169.254/v1"), key)
        .contains(QStringLiteral("SSRF"), Qt::CaseInsensitive));
    // Cleartext-remote Bearer (ANTS-1826/2108).
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("http://example.com/v1"), key)
        .contains(QStringLiteral("cleartext"), Qt::CaseInsensitive));
    // No key → cleartext-remote is allowed (the key is what must not leak).
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("http://example.com/v1"), QString()).isEmpty());
    // Precedence: userinfo reported before the SSRF gate on the same URL.
    EXPECT_TRUE(LlmClient::endpointEgressError(
        QStringLiteral("http://user:pass@169.254.169.254/v1"), key)
        .contains(QStringLiteral("credential"), Qt::CaseInsensitive));
}

// ANTS-2121 INV-8 (wiring) — the AuditDialog AI-triage POSTs build a raw
// QNetworkAccessManager rather than routing through LlmClient::send, so they
// must call endpointEgressError AND set ManualRedirectPolicy to enforce the
// same egress policy. Source-grep guard against a regression that reintroduces
// the partial scheme+cleartext-only check this path used to duplicate.
TEST(LlmClient, Ants2121_AuditTriageRoutesThroughEgressValidator) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "could not read " << SRC_AUDITDIALOG_CPP_PATH;
    // Both triage POSTs (single + batch) call the shared validator.
    const std::string needle = "LlmClient::endpointEgressError";
    const size_t first = src.find(needle);
    ASSERT_NE(first, std::string::npos)
        << "audit triage must call endpointEgressError";
    EXPECT_NE(src.find(needle, first + needle.size()), std::string::npos)
        << "both requestAiTriage and requestAiTriageBatch must call it";
    // ...and both refuse redirects (the guard endpointEgressError can't carry).
    const std::string mrp = "ManualRedirectPolicy";
    const size_t firstMrp = src.find(mrp);
    ASSERT_NE(firstMrp, std::string::npos)
        << "audit triage must set ManualRedirectPolicy";
    EXPECT_NE(src.find(mrp, firstMrp + mrp.size()), std::string::npos)
        << "both triage POSTs must set ManualRedirectPolicy";
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
