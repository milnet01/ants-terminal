// ANTS-1422 — feature-conformance test for the token_usage
// dispatch signature.
//
// Pull 3 closeout (2026-05-16) — the no_claude_integration /
// no_main diagnostic envelope and the `m_main->claudeIntegration()`
// fallback path were retired. The only caller (the MCP lambda in
// mainwindow.cpp) always supplies the captured ClaudeIntegration*,
// and the indirection was observed null on a live build with no
// static-analysis explanation. These invariants lock in the
// simplified shape: cmdTokenUsage takes one mandatory ci, no
// fallback, no diagnostic-envelope debug fields.
//
// Directory name is retained from pulls 1/2 (was the
// "no_ci_diagnostic" fixture) to keep the CMake target stable;
// the test contents now reflect the post-pull-3 contract.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — cmdTokenUsage signature takes a single mandatory
// ClaudeIntegration* parameter (no default, no lambdaThisPtr).
TEST(token_usage_no_ci_diagnostic, Inv1SimplifiedSignature) {
    expect_reset();
    const std::string h = ants_test::slurpFile(SRC_RC_HEADER);
    expect(contains(h,
        "QJsonDocument cmdTokenUsage(const QJsonObject &req,"),
           "INV-1: cmdTokenUsage signature line stable");
    expect(contains(h, "ClaudeIntegration *ci);"),
           "INV-1: cmdTokenUsage takes a single mandatory "
           "ClaudeIntegration *ci parameter");
    expect(!contains(h, "lambdaThisPtr"),
           "INV-1: lambdaThisPtr parameter retired in pull 3");
    expect(!contains(h, "explicitCi = nullptr"),
           "INV-1: explicitCi default value retired in pull 3");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — diagnostic envelopes retired. No `debug` blocks,
// no `m_main_ptr` / `this_rc_ptr` / `ci_via_getter_null` fields.
TEST(token_usage_no_ci_diagnostic, Inv2DiagnosticEnvelopesRetired) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Find cmdTokenUsage body bounds.
    const auto start = cpp.find(
        "RemoteControl::cmdTokenUsage(const QJsonObject &req,");
    ASSERT_NE(start, std::string::npos)
        << "INV-2 precondition: cmdTokenUsage definition missing";
    // 3000 bytes from the signature covers the full function body
    // without bleeding into the next function.
    const std::string region = cpp.substr(start, 3000);
    expect(!contains(region, "\"m_main_ptr\""),
           "INV-2: m_main_ptr debug field retired");
    expect(!contains(region, "\"ci_via_getter_null\""),
           "INV-2: ci_via_getter_null debug field retired");
    expect(!contains(region, "env[\"debug\"]"),
           "INV-2: env[\"debug\"] assignment retired");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — m_main->claudeIntegration() fallback retired inside
// cmdTokenUsage. The lookup was the observed-null path; with the
// only caller supplying explicitCi, the fallback is dead code.
TEST(token_usage_no_ci_diagnostic, Inv3FallbackRetired) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto start = cpp.find(
        "RemoteControl::cmdTokenUsage(const QJsonObject &req,");
    ASSERT_NE(start, std::string::npos)
        << "INV-3 precondition: cmdTokenUsage definition missing";
    const std::string region = cpp.substr(start, 3000);
    expect(!contains(region, "m_main->claudeIntegration()"),
           "INV-3: m_main->claudeIntegration() fallback retired "
           "(only the lambda calls this; explicitCi is canonical)");
    expect(!contains(region, "\"no_claude_integration\""),
           "INV-3: no_claude_integration literal retired");
    expect(!contains(region, "\"no_main\""),
           "INV-3: no_main literal retired");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — success path clean: no debug field, success envelope
// unchanged from the pre-1422 shape.
TEST(token_usage_no_ci_diagnostic, Inv4SuccessPathClean) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("env[\"ok\"] = true;");
    ASSERT_NE(pos, std::string::npos)
        << "INV-4 precondition: cmdTokenUsage success path "
           "missing (env[\"ok\"] = true; not found)";
    const std::string region = cpp.substr(pos, 2000);
    expect(!contains(region, "env[\"debug\"]"),
           "INV-4: success path must not assign a debug field");
    expect(contains(region, "env[\"calls\"]"),
           "INV-4: success path assigns the calls array");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — lambda passes m_claudeIntegration directly. No
// lambdaThisPtr forwarding (the diagnostic envelope is gone).
TEST(token_usage_no_ci_diagnostic, Inv5LambdaPassesCiDirectly) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const auto pos = mw.find(
        "registerToolProvider(\"token_usage\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-5 precondition: token_usage registration missing "
           "from mainwindow.cpp";
    // 600-byte window covers the (now smaller) lambda body.
    const std::string region = mw.substr(pos, 600);
    expect(contains(region,
        "cmdTokenUsage(args, m_claudeIntegration)"),
           "INV-5: lambda calls cmdTokenUsage with args + ci only");
    expect(!contains(region, "reinterpret_cast<quintptr>(this)"),
           "INV-5: lambdaThisPtr forwarding retired in pull 3");
    EXPECT_EQ(0, expect_failures());
}
