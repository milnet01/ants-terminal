// Feature-conformance test for spec.md (ANTS-1826 / ANTS-1830).
//
// Source-grep verification that the AuditDialog AI-triage POST refuses to send
// the API key over cleartext (ANTS-1826) and that the verdict-badge title
// attribute carrying untrusted aiReasoning is double-quoted (ANTS-1830).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 (ANTS-1826, superseded by ANTS-2121) — the AI-triage POST refuses to
// send the Bearer key over cleartext. ANTS-2121 folded that refusal — plus the
// scheme / SSRF / URL-userinfo gates — into the shared
// LlmClient::endpointEgressError validator, so the auditdialog path now enforces
// it by routing through that helper rather than an inline isPlaintextRemote
// check. The cleartext predicate + message now live in llmclient.cpp (contract
// covered by LlmClient.Ants2121_EndpointEgressError).
TEST(AuditDialogRenderHardening, AiTriageRefusesCleartextKey) {
    const std::string src = ants_test::slurpAuditDialog();
    ASSERT_FALSE(src.empty());

    EXPECT_TRUE(contains(src, "LlmClient::endpointEgressError("))
        << "AI-triage POST must enforce the egress policy (incl. cleartext-key "
           "refusal) via the shared LlmClient::endpointEgressError validator";
}

// INV-3 (ANTS-2108, ANTS-2121, ANTS-6174) — every AI-triage POST goes through
// AuditDialog::postTriageRequest, which runs the shared egress validator and
// refuses redirects. Neither triage path routes through LlmClient::send, so
// the chokepoint guard in send() does not cover them. Comments are stripped:
// a comment naming the validator must not stand in for the call.
TEST(AuditDialogRenderHardening, BothAiTriagePathsGuardCleartext) {
    const std::string src = ants_test::slurpAuditDialog();
    ASSERT_FALSE(src.empty());

    const std::string builder = ants_test::stripComments(ants_test::slurpFunctionBody(
        src, "QNetworkReply *AuditDialog::postTriageRequest("));
    ASSERT_FALSE(builder.empty()) << "the one AI-triage POST builder is missing";
    EXPECT_TRUE(contains(builder, "LlmClient::endpointEgressError("));
    EXPECT_TRUE(contains(builder, "QNetworkRequest::ManualRedirectPolicy"));

    for (const char *sig : {"void AuditDialog::requestAiTriage(",
                            "void AuditDialog::requestAiTriageBatch("}) {
        const std::string body =
            ants_test::stripComments(ants_test::slurpFunctionBody(src, sig));
        ASSERT_FALSE(body.empty()) << sig;
        EXPECT_TRUE(contains(body, "postTriageRequest(")) << sig;
        EXPECT_FALSE(contains(body, "->post("))
            << sig << " must post only through postTriageRequest";
    }
}

// INV-2 (ANTS-1830) — verdict-badge title is double-quoted, not single-quoted.
TEST(AuditDialogRenderHardening, VerdictBadgeTitleIsDoubleQuoted) {
    const std::string src = ants_test::slurpAuditDialog();
    ASSERT_FALSE(src.empty());

    // The pre-fix single-quoted form must be gone.
    EXPECT_FALSE(contains(src, "title='AI triage:"))
        << "verdict-badge title must not be single-quoted (toHtmlEscaped leaves "
           "' unescaped, allowing attribute breakout)";
    // The escaping of the untrusted reasoning is still applied.
    EXPECT_TRUE(contains(src, "f.aiReasoning.left(160).toHtmlEscaped()"))
        << "the untrusted reasoning must still be HTML-escaped";
}
