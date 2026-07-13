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

// INV-1 (ANTS-1826) — cleartext-key refusal in the AI-triage POST.
TEST(AuditDialogRenderHardening, AiTriageRefusesCleartextKey) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());

    // Reuses the shared LlmClient predicate rather than re-deriving the test.
    EXPECT_TRUE(contains(src, "LlmClient::isPlaintextRemote("))
        << "AI-triage POST must consult LlmClient::isPlaintextRemote";
    // Gated on a non-empty API key (nothing to leak otherwise).
    EXPECT_TRUE(contains(src, "!apiKey.isEmpty() && LlmClient::isPlaintextRemote"))
        << "the cleartext refusal must be gated on a configured API key";
    // The user-facing refusal message is present.
    EXPECT_TRUE(contains(src, "refusing to send the API key over cleartext"))
        << "a clear refusal message must be surfaced";
}

// INV-3 (ANTS-2108) — BOTH raw-QNAM AI-triage paths guard cleartext. The
// single-finding path (requestAiTriage, ANTS-1826) and the batch path
// (requestAiTriageBatch) each use their own QNetworkAccessManager rather than
// LlmClient::send, so the chokepoint guard in LlmClient does not cover them —
// each must gate independently. Pre-2108 only the single path was guarded.
TEST(AuditDialogRenderHardening, BothAiTriagePathsGuardCleartext) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());

    EXPECT_GE(countOccurrences(src, "!apiKey.isEmpty() && LlmClient::isPlaintextRemote"), 2)
        << "both the single-finding and batch AI-triage POSTs must gate the "
           "cleartext-key refusal (each uses a raw QNetworkAccessManager)";
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
