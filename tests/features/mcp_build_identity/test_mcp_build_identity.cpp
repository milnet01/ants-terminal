// ANTS-1952 — feature-conformance test for MCP build-identity surfacing.
// Source-scrape pattern (cf. mcp_tool_info_verb): locks the wiring that
// stamps the git SHA + build time into serverInfo / get_session_info so a
// caller can detect a ship-vs-live binary gap.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CMAKELISTS_PATH
#error "SRC_CMAKELISTS_PATH compile definition required"
#endif

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Extract the substring between the first occurrence of `from` and the
// next occurrence of `to` after it. Returns "" if either marker missing.
std::string between(const std::string &hay, const std::string &from,
                    const std::string &to) {
    const auto a = hay.find(from);
    if (a == std::string::npos) return {};
    const auto b = hay.find(to, a);
    if (b == std::string::npos) return {};
    return hay.substr(a, b - a);
}

}  // namespace

// INV-1 — the generated build_info.h header is included.
TEST(mcp_build_identity, Inv1IncludesBuildInfo) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "#include \"build_info.h\""),
           "INV-1: claudeintegration.cpp must #include build_info.h");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — the initialize serverInfo object carries all four build
// identity fields, each from the matching ANTS_BUILD_* macro.
TEST(mcp_build_identity, Inv2InitializeServerInfoStamped) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Scope to the initialize serverInfo construction so we are not
    // fooled by the get_session_info copy below.
    const std::string region =
        between(ci, "serverInfo[\"name\"]", "result[\"protocolVersion\"]");
    ASSERT_FALSE(region.empty())
        << "INV-2 precondition: initialize serverInfo block not found";
    expect(contains(region, "serverInfo[\"build_commit\"]") &&
               contains(region, "ANTS_BUILD_COMMIT"),
           "INV-2: build_commit stamped from ANTS_BUILD_COMMIT");
    expect(contains(region, "serverInfo[\"build_date\"]") &&
               contains(region, "ANTS_BUILD_DATE"),
           "INV-2: build_date stamped from ANTS_BUILD_DATE");
    expect(contains(region, "serverInfo[\"build_time\"]") &&
               contains(region, "ANTS_BUILD_TIME"),
           "INV-2: build_time stamped from ANTS_BUILD_TIME");
    expect(contains(region, "serverInfo[\"build_type\"]") &&
               contains(region, "ANTS_BUILD_TYPE"),
           "INV-2: build_type stamped from ANTS_BUILD_TYPE");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — get_session_info re-surfaces the same identity under
// server_build_* keys.
TEST(mcp_build_identity, Inv3SessionInfoResurfacesIdentity) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string region =
        between(ci, "toolName == \"get_session_info\"", "toolHandled = true;");
    ASSERT_FALSE(region.empty())
        << "INV-3 precondition: get_session_info branch not found";
    expect(contains(region, "info[\"server_build_commit\"]") &&
               contains(region, "ANTS_BUILD_COMMIT"),
           "INV-3: server_build_commit re-surfaced in get_session_info");
    expect(contains(region, "info[\"server_build_date\"]"),
           "INV-3: server_build_date re-surfaced");
    expect(contains(region, "info[\"server_build_time\"]"),
           "INV-3: server_build_time re-surfaced");
    expect(contains(region, "info[\"server_build_type\"]"),
           "INV-3: server_build_type re-surfaced");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — CMakeLists wires the generated header dir + build-info
// dependency onto ants_claude_lib so the include resolves and the
// stamp refreshes on every build.
TEST(mcp_build_identity, Inv4CMakeWiresGeneratedHeader) {
    expect_reset();
    const std::string cml = ants_test::slurpFile(SRC_CMAKELISTS_PATH);
    const std::string region =
        between(cml, "add_library(ants_claude_lib", "add_library(ants_audit_lib");
    ASSERT_FALSE(region.empty())
        << "INV-4 precondition: ants_claude_lib block not found";
    expect(contains(region, "target_include_directories(ants_claude_lib") &&
               contains(region, "generated"),
           "INV-4: ants_claude_lib gets the build/generated include dir");
    expect(contains(region, "add_dependencies(ants_claude_lib ants_build_info)"),
           "INV-4: ants_claude_lib depends on ants_build_info");
    EXPECT_EQ(0, expect_failures());
}
