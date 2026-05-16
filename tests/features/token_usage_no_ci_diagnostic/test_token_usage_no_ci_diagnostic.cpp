// ANTS-1422 — feature-conformance test for the token_usage
// no_claude_integration / no_main diagnostic envelope.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — no_ci branch emits debug object with the three fields.
TEST(token_usage_no_ci_diagnostic, Inv1NoCiDiagnostic) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("\"no_claude_integration\"");
    ASSERT_NE(pos, std::string::npos)
        << "INV-1 precondition: no_claude_integration literal "
           "missing from remotecontrol.cpp";
    // 1500 bytes covers the inline envelope + debug block.
    const std::string region = cpp.substr(pos, 1500);
    expect(contains(region, "\"m_main_ptr\""),
           "INV-1: debug.m_main_ptr emitted on no_ci branch");
    expect(contains(region, "\"this_rc_ptr\""),
           "INV-1: debug.this_rc_ptr emitted on no_ci branch");
    expect(contains(region, "\"ci_via_getter_null\""),
           "INV-1: debug.ci_via_getter_null emitted on no_ci branch");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — no_main branch emits the same diagnostic shape.
// Anchor on cmdTokenUsage's ANTS-1422 comment so the test doesn't
// confuse this with cmdRoadmapLog's own m_main null check
// (ANTS-1424, also added in Bundle C).
TEST(token_usage_no_ci_diagnostic, Inv2NoMainDiagnostic) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find(
        "cmdTokenUsage(const QJsonObject");
    ASSERT_NE(pos, std::string::npos)
        << "INV-2 precondition: cmdTokenUsage signature missing "
           "from remotecontrol.cpp";
    // 1500 bytes from the signature covers the no_main branch
    // (first branch inside cmdTokenUsage) without bleeding into
    // any sibling function.
    const std::string region = cpp.substr(pos, 1500);
    expect(contains(region, "\"no_main\""),
           "INV-2: cmdTokenUsage's no_main branch present");
    expect(contains(region, "\"m_main_ptr\""),
           "INV-2: debug.m_main_ptr emitted on no_main branch");
    expect(contains(region, "\"this_rc_ptr\""),
           "INV-2: debug.this_rc_ptr emitted on no_main branch");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — legacy tuErr helper retired.
TEST(token_usage_no_ci_diagnostic, Inv3TuErrRetired) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(!contains(cpp,
        "QJsonDocument tuErr(const QString &code,"),
           "INV-3: tuErr(const QString&, const QString&) definition "
           "must be retired — both error branches emit inline "
           "envelopes with diagnostic fields");
    expect(contains(cpp, "tuErr() helper retired"),
           "INV-3: retirement comment present at cmdTokenUsage");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — success path unchanged: no debug field.
TEST(token_usage_no_ci_diagnostic, Inv4SuccessPathClean) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const auto pos = cpp.find("env[\"ok\"] = true;");
    ASSERT_NE(pos, std::string::npos)
        << "INV-4 precondition: cmdTokenUsage success path "
           "missing (env[\"ok\"] = true; not found)";
    // 2000 bytes from `env["ok"] = true;` covers the full success
    // envelope build.
    const std::string region = cpp.substr(pos, 2000);
    expect(!contains(region, "env[\"debug\"]"),
           "INV-4: success path must not assign a debug field");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — both error branches set code == error.
TEST(token_usage_no_ci_diagnostic, Inv5CodeMatchesError) {
    expect_reset();
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // Both branches should have env["error"] AND env["code"]
    // assignments — anchor on the diagnostic comment then check
    // both fields are set with the right literals.
    const auto pos = cpp.find("ANTS-1422 — observed 2026-05-16");
    ASSERT_NE(pos, std::string::npos)
        << "INV-5 precondition: ANTS-1422 anchor comment missing "
           "from no_ci branch";
    const std::string region = cpp.substr(pos, 1500);
    expect(contains(region,
        "env[\"error\"]   = QStringLiteral(\"no_claude_integration\")"),
           "INV-5: no_ci branch sets env[\"error\"] to the "
           "no_claude_integration literal");
    expect(contains(region,
        "env[\"code\"]    = QStringLiteral(\"no_claude_integration\")"),
           "INV-5: no_ci branch sets env[\"code\"] equal to "
           "env[\"error\"]");
    EXPECT_EQ(0, expect_failures());
}
