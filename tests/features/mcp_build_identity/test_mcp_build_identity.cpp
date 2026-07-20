// ANTS-1952 — feature-conformance test for MCP build-identity surfacing.
// Source-scrape pattern (cf. mcp_tool_info_verb): locks the wiring that
// stamps the git SHA + build time into serverInfo / get_session_info so a
// caller can detect a ship-vs-live binary gap.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CMAKELISTS_PATH
#error "SRC_CMAKELISTS_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_BUILD_INFO_H_PATH
#error "SRC_BUILD_INFO_H_PATH compile definition required"
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

// INV-1 — the build_info.h header is included (ANTS-3582: now a stable
// hand-written extern-declaration header in src/, not a generated one).
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

// INV-4 (ANTS-3582) — CMakeLists refreshes the build-info VALUES every build
// via a file-level custom command feeding a generated build_info_values.cpp,
// NOT a target-level dependency on an always-run target (which forced a full
// rebuild — the reverted attempt-1 dead ends). The stamp target drives the
// per-build refresh; copy_if_different keeps only build_info_values.o dirty.
TEST(mcp_build_identity, Inv4CMakeRefreshesValuesFileLevel) {
    expect_reset();
    const std::string cml = ants_test::slurpFile(SRC_CMAKELISTS_PATH);
    expect(contains(cml, "add_custom_target(ants_build_info_stamp"),
           "INV-4: a phony stamp target drives the per-build values refresh");
    expect(contains(cml, "build_info_values.cpp") &&
               contains(cml, "GenerateBuildInfoValues.cmake"),
           "INV-4: build_info_values.cpp is generated via the values script");
    // The load-bearing negative: the values source must NOT be dragged in by a
    // target-level dependency on the always-run stamp/generator target.
    expect(!contains(cml, "add_dependencies(ants_core_lib ants_build_info_stamp)") &&
               !contains(cml, "add_dependencies(ants_claude_lib ants_build_info"),
           "INV-4: no target-level add_dependencies on the always-run refresh "
           "(that forced a full rebuild in the reverted attempt)");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 (ANTS-2073) — remotecontrol.cpp includes build_info.h and
// cmdSessionOrient stamps a server_build block from the macros.
TEST(mcp_build_identity, Inv5SessionOrientStampsServerBuild) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(rc, "#include \"build_info.h\""),
           "INV-5: remotecontrol.cpp must #include build_info.h");
    const std::string region =
        between(rc, "RemoteControl::cmdSessionOrient", "spec-aware MCP tools");
    ASSERT_FALSE(region.empty())
        << "INV-5 precondition: cmdSessionOrient body not found";
    expect(contains(region, "server_build") &&
               contains(region, "ANTS_BUILD_COMMIT") &&
               contains(region, "ANTS_VERSION"),
           "INV-5: session_orient adds server_build from ANTS_BUILD_* + version");
    // ANTS-2152 — the block carries a stale_check_hint steering clients to
    // compare build_commit/build_date (not version) before re-reporting a
    // shipped item as still broken.
    expect(contains(region, "stale_check_hint"),
           "INV-5: server_build carries the ANTS-2152 stale_check_hint");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 (ANTS-2073) — the tool_info catalog branch stamps server_build.
TEST(mcp_build_identity, Inv6ToolInfoCatalogStampsServerBuild) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string region =
        between(ci, "env[\"category_count\"]", "reqName.isEmpty()");
    ASSERT_FALSE(region.empty())
        << "INV-6 precondition: tool_info catalog branch not found";
    expect(contains(region, "server_build") &&
               contains(region, "ANTS_BUILD_COMMIT"),
           "INV-6: tool_info catalog stamps server_build from ANTS_BUILD_*");
    EXPECT_EQ(0, expect_failures());
}

// INV-7 (ANTS-2073 + ANTS-3582) — build_info_values.cpp (the extern
// definitions) compiles into ants_core_lib, where remotecontrol.cpp lives, so
// the symbols resolve for every consumer that links core. SKIP_PRECOMPILE_HEADERS
// keeps a per-build value change from dragging core's PCH.
TEST(mcp_build_identity, Inv7CMakeCompilesValuesIntoCoreLib) {
    expect_reset();
    const std::string cml = ants_test::slurpFile(SRC_CMAKELISTS_PATH);
    expect(contains(cml, "target_sources(ants_core_lib") &&
               contains(cml, "ANTS_BUILD_INFO_VALUES_CPP"),
           "INV-7: build_info_values.cpp compiles into ants_core_lib");
    expect(contains(cml, "SKIP_PRECOMPILE_HEADERS"),
           "INV-7: the generated values TU is PCH-decoupled so a value change "
           "doesn't rebuild the core PCH");
    EXPECT_EQ(0, expect_failures());
}

// INV-8 (ANTS-3582) — churn regression guard. src/build_info.h must be the
// STABLE extern-declaration header, NOT a macro header that bakes the build
// minute into the preprocessed form ccache hashes (which recompiled the two
// ~1 MB consumers on every cross-minute build). A regression back to
// `#define ANTS_BUILD_TIME "HH:MM"` in the header is what this catches.
TEST(mcp_build_identity, Inv8BuildInfoHeaderIsStableExternDecls) {
    expect_reset();
    const std::string bi = ants_test::slurpFile(SRC_BUILD_INFO_H_PATH);
    expect(contains(bi, "extern const char ANTS_BUILD_TIME[]"),
           "INV-8: build_info.h declares ANTS_BUILD_TIME as an extern array");
    expect(!contains(bi, "#define ANTS_BUILD_TIME"),
           "INV-8: build_info.h must NOT bake the build minute in as a macro "
           "(that re-introduces the cross-minute recompile of the heavy TUs)");
    expect(!contains(bi, "@ANTS_BUILD_TIME@"),
           "INV-8: build_info.h is hand-written + stable, not a configure "
           "template (the values live in the generated build_info_values.cpp)");
    EXPECT_EQ(0, expect_failures());
}
