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

int countOccurrences(const std::string &hay, const char *needle) {
    int n = 0;
    const std::string pat(needle);
    for (size_t pos = hay.find(pat); pos != std::string::npos;
         pos = hay.find(pat, pos + pat.size()))
        ++n;
    return n;
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
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());

    EXPECT_TRUE(contains(src, "LlmClient::endpointEgressError("))
        << "AI-triage POST must enforce the egress policy (incl. cleartext-key "
           "refusal) via the shared LlmClient::endpointEgressError validator";
}

// INV-3 (ANTS-2108, superseded by ANTS-2121) — BOTH raw-QNAM AI-triage paths
// enforce the egress policy. The single-finding path (requestAiTriage) and the
// batch path (requestAiTriageBatch) each use their own QNetworkAccessManager
// rather than LlmClient::send, so the chokepoint guard in send() does not cover
// them — each must run endpointEgressError independently. ANTS-2121 replaced the
// former per-path inline cleartext check with this shared validator (which also
// adds the SSRF / userinfo / scheme gates + the ManualRedirectPolicy below it).
TEST(AuditDialogRenderHardening, BothAiTriagePathsGuardCleartext) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());

    EXPECT_GE(countOccurrences(src, "LlmClient::endpointEgressError("), 2)
        << "both the single-finding and batch AI-triage POSTs must run the "
           "shared egress validator (each uses a raw QNetworkAccessManager)";
}

// INV-2 (ANTS-1830) — verdict-badge title is double-quoted, not single-quoted.
TEST(AuditDialogRenderHardening, VerdictBadgeTitleIsDoubleQuoted) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());

    // The pre-fix single-quoted form must be gone.
    EXPECT_FALSE(contains(src, "title='AI triage:"))
        << "verdict-badge title must not be single-quoted (toHtmlEscaped leaves "
           "' unescaped, allowing attribute breakout)";
    // The escaping of the untrusted reasoning is still applied.
    EXPECT_TRUE(contains(src, "f.aiReasoning.left(160).toHtmlEscaped()"))
        << "the untrusted reasoning must still be HTML-escaped";
}
