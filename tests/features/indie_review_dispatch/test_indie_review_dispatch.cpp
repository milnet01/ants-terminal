// ANTS-1352 — feature-conformance test for indie_review_dispatch.
// Source-grep tests against the engine + MCP wiring + cold-eyes-
// folded invariants. No live API; live-API testing is in the
// manual recipe (spec § 7.2).

#include "../../_support/srcgrep.h"

#include "indiereviewdispatcher.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QSet>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <string>
#include <utility>

namespace {

const char *kEnginePath  = SRC_INDIE_REVIEW_DISPATCHER_CPP_PATH;
const char *kMainPath    = SRC_MAINWINDOW_CPP_PATH;
const char *kCiPath      = SRC_CLAUDE_INTEGRATION_CPP_PATH;
const char *kEngineHdr   = SRC_INDIE_REVIEW_DISPATCHER_H_PATH;
const char *kIreHdr      = SRC_INDIE_REVIEW_ENGINE_H_PATH;
const char *kIreCpp      = SRC_INDIE_REVIEW_ENGINE_CPP_PATH;
const char *kErrorsDoc   = MCP_ERROR_CODES_DOC_PATH;

}  // namespace

namespace {

// ---------------------------------------------------------------------
// SignalHttpServer — ANTS-5018 INV-4. A tiny loopback HTTP server
// serviced ENTIRELY by signal/slot connections (QTcpServer::newConnection
// + the accepted socket's readyRead), never by polling.
//
// dispatchLanes() (ANTS-1352) spins its OWN nested QEventLoop (see
// indiereviewdispatcher.cpp's file banner) rather than yielding control
// back to the caller between requests. The llm_client FakeHttpServer's
// waitUntil() technique — call processEvents() from the caller in a
// spin loop — cannot service a server while dispatchLanes() itself is
// blocked inside that nested loop.exec(): the caller never gets the
// thread back until dispatchLanes() returns. But Qt's event dispatcher
// delivers socket signals to every QObject affine to the SAME thread
// regardless of which nested QEventLoop currently owns it — so a server
// wired purely by connect(), with no polling, IS serviced correctly from
// inside dispatchLanes()'s own loop.exec(), as long as it lives on the
// calling (test) thread. It does: this class is constructed directly in
// the TEST body, never handed to another thread.
class SignalHttpServer {
public:
    explicit SignalHttpServer(QByteArray response)
        : m_response(std::move(response)) {
        m_listening = m_server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&m_server, &QTcpServer::newConnection,
                          &m_server, [this] { onNewConnection(); });
    }

    // Accepted sockets are children of m_server, which is destroyed after
    // the maps that index them. Delete them first, so no readyRead lambda
    // capturing `this` can outlive those maps.
    ~SignalHttpServer() {
        for (QTcpSocket *s : std::as_const(m_sockets)) delete s;
    }

    bool isListening() const { return m_listening; }
    quint16 port() const { return m_server.serverPort(); }
    int requestCount() const { return m_requestCount; }

private:
    void onNewConnection() {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *socket = m_server.nextPendingConnection();
            m_sockets.append(socket);
            QObject::connect(socket, &QTcpSocket::readyRead,
                              socket, [this, socket] { onReadyRead(socket); });
        }
    }

    void onReadyRead(QTcpSocket *socket) {
        if (m_responded.contains(socket)) return;
        m_buffers[socket] += socket->readAll();
        const QByteArray &buf = m_buffers[socket];
        const int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;  // headers not fully received yet

        qint64 contentLength = 0;
        const QList<QByteArray> headerLines = buf.left(headerEnd).split('\n');
        for (const QByteArray &line : headerLines) {
            if (line.toLower().startsWith("content-length:")) {
                contentLength =
                    line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                break;
            }
        }
        const qint64 bodyStart = headerEnd + 4;
        if (buf.size() < bodyStart + contentLength)
            return;  // body not fully received yet

        m_responded.insert(socket);
        ++m_requestCount;
        socket->write(m_response);
        socket->disconnectFromHost();
    }

    QByteArray                      m_response;
    bool                            m_listening = false;
    QTcpServer                      m_server;
    QList<QTcpSocket *>             m_sockets;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QSet<QTcpSocket *>              m_responded;
    int                             m_requestCount = 0;
};

// Minimal one-lane DispatchRequest against a fresh QTemporaryDir. Callers
// override endpoint / apiKey / perLaneTimeoutMs as needed.
//
// perLaneTimeoutMs is bounded to 3s (production default is 300000ms /
// 5 min): INV-1..INV-3 assert that dispatchLanes refuses BEFORE any
// network traffic, but that is exactly the invariant that is false today
// (ANTS-5018) — against current code these endpoints are NOT refused and
// the dispatcher actually attempts to reach them. A short bound keeps a
// red run's wall-clock cost small and deterministic regardless of
// whether the test host has outbound network access, rather than
// blocking on the production timeout or an OS-level connect timeout.
IndieReviewDispatcher::DispatchRequest makeOneLaneRequest(
        const QString &projectRoot, const QString &endpoint,
        const QString &apiKey) {
    IndieReviewDispatcher::DispatchRequest req;
    req.projectRoot      = projectRoot;
    req.reportsDir       = QStringLiteral("reports");
    req.endpoint         = endpoint;
    req.apiKey           = apiKey;
    req.model            = QStringLiteral("gpt-4");
    req.systemPrompt     = QStringLiteral("system prompt");
    req.perLaneTimeoutMs = 3000;
    IndieReviewDispatcher::LaneRequest lane;
    lane.name  = QStringLiteral("lane1");
    lane.brief = QStringLiteral("lane brief");
    req.lanes.append(lane);
    return req;
}

}  // namespace

// G-1 — dispatchLanes declared in header.
TEST(IndieReviewDispatch, G1_DispatcherDeclared) {
    const std::string h = ants_test::slurpFile(kEngineHdr);
    ASSERT_FALSE(h.empty());
    EXPECT_NE(h.find("DispatchResult dispatchLanes"), std::string::npos);
}

// G-2 — contract Required.
TEST(IndieReviewDispatch, G2_ContractRequired) {
    const std::string cc = ants_test::slurpFile(kCiPath);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("// ANTS-1352: indie_review_dispatch");
    ASSERT_NE(pos, std::string::npos);
    const std::string region = cc.substr(pos, 400);
    EXPECT_NE(region.find("\"indie_review_dispatch\""),
              std::string::npos);
    EXPECT_NE(region.find("C::Required"), std::string::npos);
}

// G-3 — tier Expensive.
TEST(IndieReviewDispatch, G3_TierExpensive) {
    const std::string cc = ants_test::slurpFile(kCiPath);
    ASSERT_FALSE(cc.empty());
    // The tier classification has its own anchor in claudeintegration.cpp
    // — search for the tool name appearing on a line that also says
    // R::Expensive.
    const std::string needle =
        "\"indie_review_dispatch\"))    return R::Expensive";
    EXPECT_NE(cc.find(needle), std::string::npos);
}

// G-4 — provider registered.
TEST(IndieReviewDispatch, G4_ProviderRegistered) {
    const std::string mw = ants_test::slurpFile(kMainPath);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"indie_review_dispatch\""),
              std::string::npos);
}

// G-5 — in-flight gate.
TEST(IndieReviewDispatch, G5_InFlightGate) {
    const std::string mw = ants_test::slurpFile(kMainPath);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find(
        "verbInFlightTryAcquire(\n"
        "                    QStringLiteral(\"indie_review_dispatch\")"),
        std::string::npos);
    // The slot is released via a qScopeGuard so it fires on every exit
    // path (incl. exception), not just the explicit return — the release
    // call now lives one indent deeper inside the guard lambda.
    // indie-review-2026-05-21.
    EXPECT_NE(mw.find(
        "verbInFlightRelease(\n"
        "                    QStringLiteral(\"indie_review_dispatch\")"),
        std::string::npos);
    EXPECT_NE(mw.find("qScopeGuard"), std::string::npos);
}

// G-6 — PathValidation on reports_dir.
TEST(IndieReviewDispatch, G6_PathValidation) {
    const std::string handler =
        ants_test::slurpFunctionBody(
            ants_test::slurpRemoteControl(),
            "RemoteControl::cmdIndieReviewDispatch");
    ASSERT_FALSE(handler.empty());
    EXPECT_NE(handler.find("PathValidation::validatePath"),
              std::string::npos);
    EXPECT_NE(handler.find("reports_dir"), std::string::npos);
}

// G-7 — QSaveFile.
TEST(IndieReviewDispatch, G7_AtomicWrite) {
    const std::string eng = ants_test::slurpFile(kEnginePath);
    ASSERT_FALSE(eng.empty());
    EXPECT_NE(eng.find("QSaveFile"), std::string::npos);
    EXPECT_NE(eng.find(".commit()"), std::string::npos);
}

// G-8 — apiKey not in any logging call.
TEST(IndieReviewDispatch, G8_ApiKeyNotLogged) {
    const std::string eng = ants_test::slurpFile(kEnginePath);
    ASSERT_FALSE(eng.empty());
    // Walk every occurrence of `apiKey` and require its line not
    // include qDebug/qInfo/qWarning/qCritical.
    std::size_t pos = 0;
    while ((pos = eng.find("apiKey", pos)) != std::string::npos) {
        const auto lineStart = eng.rfind('\n', pos);
        const auto lineEnd   = eng.find('\n', pos);
        const std::string line = eng.substr(
            lineStart == std::string::npos ? 0 : lineStart + 1,
            (lineEnd == std::string::npos ? eng.size() : lineEnd)
                - (lineStart == std::string::npos ? 0 : lineStart + 1));
        for (const char *fn : {"qDebug", "qInfo", "qWarning", "qCritical"}) {
            EXPECT_EQ(line.find(fn), std::string::npos)
                << "G-8: apiKey appears in logging call: " << line;
        }
        pos += 6;
    }
}

// G-9 — setTransferTimeout invoked.
TEST(IndieReviewDispatch, G9_PerLaneTimeout) {
    const std::string eng = ants_test::slurpFile(kEnginePath);
    ASSERT_FALSE(eng.empty());
    EXPECT_NE(eng.find("setTransferTimeout(req.perLaneTimeoutMs)"),
              std::string::npos);
}

// G-10 — refusal envelope shape (code field on every refusal).
TEST(IndieReviewDispatch, G10_RefusalEnvelopeShape) {
    const std::string handler =
        ants_test::slurpFunctionBody(
            ants_test::slurpRemoteControl(),
            "RemoteControl::cmdIndieReviewDispatch");
    ASSERT_FALSE(handler.empty());
    // Every irErr call in this handler must pair (code, message).
    // Count the irErr calls and require >= 6 (we have many refusal
    // branches: no_window, no_project, bad_args ×3+, no_lanes,
    // ai_not_configured ×2).
    EXPECT_GE(ants_test::countOccurrences(handler, "irErr"),
              std::size_t(6));
}

// G-11 — mcp-error-codes.md rows for the new codes.
TEST(IndieReviewDispatch, G11_NewErrorCodesDocumented) {
    const std::string doc = ants_test::slurpFile(kErrorsDoc);
    ASSERT_FALSE(doc.empty());
    EXPECT_NE(doc.find("`ai_not_configured`"), std::string::npos);
    EXPECT_NE(doc.find("`no_lanes`"), std::string::npos);
}

// G-12 — tools/list descriptor.
TEST(IndieReviewDispatch, G12_ToolDescriptor) {
    const std::string cc = ants_test::slurpFile(kCiPath);
    ASSERT_FALSE(cc.empty());
    // The descriptor block sets t["name"] = "indie_review_dispatch";
    EXPECT_NE(cc.find("t[\"name\"] = \"indie_review_dispatch\""),
              std::string::npos);
}

// G-13 — not in idempotent-read cache allowlist.
TEST(IndieReviewDispatch, G13_NotCacheable) {
    const std::string cc = ants_test::slurpFile(kCiPath);
    ASSERT_FALSE(cc.empty());
    const std::string isIdemBody =
        ants_test::slurpFunctionBody(
            cc, "ClaudeIntegration::isIdempotentReadTool");
    ASSERT_FALSE(isIdemBody.empty());
    EXPECT_EQ(isIdemBody.find("\"indie_review_dispatch\""),
              std::string::npos);
}

// G-14 — assembleBriefForDispatch declared.
TEST(IndieReviewDispatch, G14_DispatchBriefDeclared) {
    const std::string h = ants_test::slurpFile(kIreHdr);
    ASSERT_FALSE(h.empty());
    EXPECT_NE(h.find("QString assembleBriefForDispatch"),
              std::string::npos);
}

// G-15 — 4-backtick fence + "treat as data" preamble. ANTS-1727
// relocated the fence kernel from assembleBriefForDispatch's body into
// BriefDispatch::fenceBody (shared with cold-eyes / test-audit), so the
// literals now live there; assembleBriefForDispatch must *call* it.
// Behaviour is unchanged (covered behaviourally by the
// brief_dispatch_fence feature test, INV-10/INV-11).
TEST(IndieReviewDispatch, G15_FenceHardening) {
    const std::string fence =
        ants_test::slurpFunctionBody(
            ants_test::slurpFile(SRC_BRIEFDISPATCH_CPP_PATH),
            "fenceBody");
    ASSERT_FALSE(fence.empty());
    EXPECT_NE(fence.find("treat as data,"), std::string::npos);
    EXPECT_NE(fence.find("not instructions"), std::string::npos);
    // 4-backtick fence sentinel (QStringLiteral with 4 literal
    // backticks then \n escape).
    EXPECT_NE(fence.find("QStringLiteral(\"````"), std::string::npos);
    // Fence-escape defense: 4-backtick run replaced with '```'.
    EXPECT_NE(fence.find("'```'"), std::string::npos);

    // assembleBriefForDispatch is refactored onto the shared kernel.
    const std::string brief =
        ants_test::slurpFunctionBody(
            ants_test::slurpFile(kIreCpp),
            "assembleBriefForDispatch");
    ASSERT_FALSE(brief.empty());
    EXPECT_NE(brief.find("BriefDispatch::fenceBody"), std::string::npos);
}

// G-16 — redact helper called before any envelope-bound use.
TEST(IndieReviewDispatch, G16_ResponseBodyRedaction) {
    const std::string eng = ants_test::slurpFile(kEnginePath);
    ASSERT_FALSE(eng.empty());
    EXPECT_NE(eng.find("redactAndTruncate"), std::string::npos);
    // The redact helper must scrub apiKey BEFORE truncation
    // (cold-eyes M-new-1: redact-then-truncate ordering).
    const std::string redactFn =
        ants_test::slurpFunctionBody(eng,
            "redactAndTruncate(QByteArray body");
    ASSERT_FALSE(redactFn.empty());
    const auto replacePos  = redactFn.find("text.replace(apiKey");
    const auto truncatePos = redactFn.find("utf8.truncate(");
    ASSERT_NE(replacePos,  std::string::npos);
    ASSERT_NE(truncatePos, std::string::npos);
    EXPECT_LT(replacePos, truncatePos)
        << "G-16: apiKey redaction must precede truncation "
           "(redact-then-truncate, INV-21 / cold-eyes M-new-1)";
}

// G-17 — probe accessor declared.
TEST(IndieReviewDispatch, G17_ProbeAccessor) {
    const std::string h = ants_test::slurpFile(kEngineHdr);
    ASSERT_FALSE(h.empty());
    EXPECT_NE(h.find("int inFlightCountForTest()"),
              std::string::npos);
}

// P-1 — probe returns 0 at rest.
TEST(IndieReviewDispatch, P1_ProbeZeroAtRest) {
    EXPECT_EQ(IndieReviewDispatcher::inFlightCountForTest(), 0);
}

// ANTS-5018 — indie_review_dispatch skips the shared AI egress checks:
// it sends the key over cleartext, posts to private/link-local/metadata
// IP literals, forwards URL userinfo credentials, and follows redirects.
// INV-1..INV-3 lock the three refusals dispatchLanes must perform BEFORE
// any network traffic or filesystem side effect (reports_dir must never
// be created); INV-4 locks the redirect refusal behaviourally against a
// loopback fixture. All four are refused already on LlmClient::send and
// the AuditDialog triage POSTs (ANTS-2121); this is the fourth channel
// that duplicates the scheme-only check instead of calling
// LlmClient::endpointEgressError.

// INV-1 — a keyed remote plain-http endpoint is refused before any
// network traffic or filesystem side effect, and the error names the
// cleartext refusal (LlmClient::endpointEgressError's "cleartext"
// reason). Per ANTS-5010, an EMPTY key against remote plain-http is
// deliberately allowed — this test keeps apiKey non-empty so it stays on
// the refused side of that boundary. 192.0.2.1 is TEST-NET-1 (RFC 5737):
// remote and public-shaped, and nothing answers it, so a red run reaches
// no host.
TEST(IndieReviewDispatch, INV1_RefusesCleartextRemoteWithKey) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString absReportsDir = tmp.path() + QStringLiteral("/reports");

    const IndieReviewDispatcher::DispatchRequest req = makeOneLaneRequest(
        tmp.path(),
        QStringLiteral("http://192.0.2.1/v1/chat/completions"),
        QStringLiteral("sk-test-key-1234"));

    const IndieReviewDispatcher::DispatchResult r =
        IndieReviewDispatcher::dispatchLanes(req);

    EXPECT_FALSE(r.ok)
        << "INV-1: a keyed request to a remote plain-http endpoint must "
           "be refused (ANTS-5018)";
    EXPECT_TRUE(r.error.contains(QStringLiteral("cleartext"),
                                  Qt::CaseInsensitive))
        << "INV-1: refusal error must name the cleartext refusal, got: \""
        << r.error.toStdString() << "\"";
    EXPECT_FALSE(QDir(absReportsDir).exists())
        << "INV-1: the refusal must happen before reports_dir is created "
           "— no filesystem side effect from a request that never should "
           "have gone out";
}

// INV-2 — a private/link-local/cloud-metadata IP-literal endpoint is
// refused before any network traffic, naming the SSRF refusal
// (LlmClient::endpointEgressError's "SSRF" reason). fe80::1 is link-local,
// blocked like the cloud-metadata range, and a connect to it without a
// scope id fails at once, so a red run reaches no host. The key is empty:
// the SSRF refusal must not depend on one.
TEST(IndieReviewDispatch, INV2_RefusesSsrfIpLiteral) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString absReportsDir = tmp.path() + QStringLiteral("/reports");

    const IndieReviewDispatcher::DispatchRequest req = makeOneLaneRequest(
        tmp.path(),
        QStringLiteral("http://[fe80::1]/v1/chat/completions"),
        QString());

    const IndieReviewDispatcher::DispatchResult r =
        IndieReviewDispatcher::dispatchLanes(req);

    EXPECT_FALSE(r.ok)
        << "INV-2: an SSRF-shaped IP-literal endpoint must be refused "
           "(ANTS-5018)";
    EXPECT_TRUE(r.error.contains(QStringLiteral("SSRF"),
                                  Qt::CaseInsensitive))
        << "INV-2: refusal error must name the SSRF refusal, got: \""
        << r.error.toStdString() << "\"";
    EXPECT_FALSE(QDir(absReportsDir).exists())
        << "INV-2: the refusal must happen before reports_dir is created";
}

// INV-3 — an endpoint embedding URL userinfo (user:pass@host) is
// refused before any network traffic, naming the embedded-credential
// refusal (LlmClient::endpointEgressError's "credential" reason). The host
// is TEST-NET-1, as in INV-1, and the key is empty: the credential refusal
// must not depend on one.
TEST(IndieReviewDispatch, INV3_RefusesUrlEmbeddedCredentials) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString absReportsDir = tmp.path() + QStringLiteral("/reports");

    const IndieReviewDispatcher::DispatchRequest req = makeOneLaneRequest(
        tmp.path(),
        QStringLiteral("https://user:pass@192.0.2.1/v1/chat/completions"),
        QString());

    const IndieReviewDispatcher::DispatchResult r =
        IndieReviewDispatcher::dispatchLanes(req);

    EXPECT_FALSE(r.ok)
        << "INV-3: an endpoint embedding URL userinfo must be refused "
           "(ANTS-5018)";
    EXPECT_TRUE(r.error.contains(QStringLiteral("credential"),
                                  Qt::CaseInsensitive))
        << "INV-3: refusal error must name the embedded-credential "
           "refusal, got: \"" << r.error.toStdString() << "\"";
    EXPECT_FALSE(QDir(absReportsDir).exists())
        << "INV-3: the refusal must happen before reports_dir is created";
}

// INV-4 — the dispatcher must not auto-follow a redirect. No
// ManualRedirectPolicy is set on the request today, so Qt's default
// NoLessSafeRedirectPolicy follows a same-safety-level (http-to-http)
// 3xx Location automatically — exactly the class ANTS-1798 closed on
// LlmClient::send and ANTS-2121 closed on the AuditDialog triage POSTs.
// A hostile or compromised configured endpoint could redirect a keyed
// POST at cloud metadata or any other host the SSRF guard never gets a
// chance to re-validate, since Qt's automatic redirect never re-runs it.
//
// Both endpoints are loopback (127.0.0.1) and therefore pass every OTHER
// egress gate even after ANTS-5018 is fixed (loopback is exempt from the
// cleartext/SSRF checks) — so this isolates the redirect-specific defect
// from the other three; a fix that only adds endpointEgressError without
// also setting ManualRedirectPolicy still fails this test.
TEST(IndieReviewDispatch, INV4_DoesNotFollowRedirect) {
    SignalHttpServer serverB(QByteArray(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{}"));
    ASSERT_TRUE(serverB.isListening());

    const QByteArray redirectResponse =
        QByteArray("HTTP/1.1 307 Temporary Redirect\r\n"
                    "Location: http://127.0.0.1:")
        + QByteArray::number(serverB.port())
        + QByteArray("/v1/chat/completions\r\n"
                      "Content-Length: 0\r\n"
                      "Connection: close\r\n"
                      "\r\n");
    SignalHttpServer serverA(redirectResponse);
    ASSERT_TRUE(serverA.isListening());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    IndieReviewDispatcher::DispatchRequest req = makeOneLaneRequest(
        tmp.path(),
        QStringLiteral("http://127.0.0.1:%1").arg(serverA.port()),
        QStringLiteral("sk-test-key-redirect"));
    // Bound the worst case (a harness bug that never answers) to a few
    // seconds rather than the 5-minute production default.
    req.perLaneTimeoutMs = 5000;

    IndieReviewDispatcher::dispatchLanes(req);

    EXPECT_EQ(serverB.requestCount(), 0)
        << "INV-4: dispatchLanes must not follow a redirect — server B "
           "(the redirect target) must never see a connection "
           "(ANTS-5018, mirrors ANTS-1798/ANTS-2121)";
    EXPECT_EQ(serverA.requestCount(), 1)
        << "sanity: the dispatcher must have reached server A at all, or "
           "the test proves nothing about redirect handling";
}
