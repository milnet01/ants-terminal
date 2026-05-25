// Feature-conformance test for spec.md (ANTS-1826 / ANTS-1830).
//
// Source-grep verification that the AuditDialog AI-triage POST refuses to send
// the API key over cleartext (ANTS-1826) and that the verdict-badge title
// attribute carrying untrusted aiReasoning is double-quoted (ANTS-1830).

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 (ANTS-1826) — cleartext-key refusal in the AI-triage POST.
TEST(AuditDialogRenderHardening, AiTriageRefusesCleartextKey) {
    const std::string src = slurp(SRC_AUDITDIALOG_PATH);
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

// INV-2 (ANTS-1830) — verdict-badge title is double-quoted, not single-quoted.
TEST(AuditDialogRenderHardening, VerdictBadgeTitleIsDoubleQuoted) {
    const std::string src = slurp(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());

    // The pre-fix single-quoted form must be gone.
    EXPECT_FALSE(contains(src, "title='AI triage:"))
        << "verdict-badge title must not be single-quoted (toHtmlEscaped leaves "
           "' unescaped, allowing attribute breakout)";
    // The escaping of the untrusted reasoning is still applied.
    EXPECT_TRUE(contains(src, "f.aiReasoning.left(160).toHtmlEscaped()"))
        << "the untrusted reasoning must still be HTML-escaped";
}
