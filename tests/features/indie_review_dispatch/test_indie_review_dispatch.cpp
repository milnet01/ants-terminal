// ANTS-1352 — feature-conformance test for indie_review_dispatch.
// Source-grep tests against the engine + MCP wiring + cold-eyes-
// folded invariants. No live API; live-API testing is in the
// manual recipe (spec § 7.2).

#include "../../_support/srcgrep.h"

#include "indiereviewdispatcher.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const char *kEnginePath  = SRC_INDIE_REVIEW_DISPATCHER_CPP_PATH;
const char *kHandlerPath = SRC_REMOTECONTROL_CPP_PATH;
const char *kMainPath    = SRC_MAINWINDOW_CPP_PATH;
const char *kCiPath      = SRC_CLAUDE_INTEGRATION_CPP_PATH;
const char *kEngineHdr   = SRC_INDIE_REVIEW_DISPATCHER_H_PATH;
const char *kIreHdr      = SRC_INDIE_REVIEW_ENGINE_H_PATH;
const char *kIreCpp      = SRC_INDIE_REVIEW_ENGINE_CPP_PATH;
const char *kErrorsDoc   = MCP_ERROR_CODES_DOC_PATH;

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
            ants_test::slurpFile(kHandlerPath),
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
            ants_test::slurpFile(kHandlerPath),
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
