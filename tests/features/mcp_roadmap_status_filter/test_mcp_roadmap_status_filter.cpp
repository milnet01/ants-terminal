// ANTS-1247 — feature-conformance test for the `status` filter on
// roadmap_query (MCP tool + IPC verb). Source-grep harness; no GUI,
// no QTemporaryDir.
//
// Each INV maps to a // ANTS-1247-INV-N anchor in source.

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const std::string &detail = {}) {
    std::fprintf(stderr, "[%-80s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 detail.empty() ? "" : " — ",
                 detail.c_str());
    if (!ok) ++g_failures;
}

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

TEST(mcp_roadmap_status_filter, Inv1BackCompatSignatureDefaulted) {
    const std::string h = slurp(SRC_RC_HEADER);
    // ANTS-1247-INV-1: signature accepts QJsonObject with default {}.
    expect(contains(h, "cmdRoadmapQuery(const QJsonObject &req = {})"),
           "INV-1: cmdRoadmapQuery has `const QJsonObject &req = {}` default param");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv2ActiveFilterSwitch) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-2: planned + in-progress emoji compare.
    expect(contains(cpp, "ANTS-1247-INV-2"),
           "INV-2 anchor comment present in remotecontrol.cpp");
    expect(contains(cpp, "plannedEmoji"),
           "INV-2: plannedEmoji constant present");
    expect(contains(cpp, "progressEmoji"),
           "INV-2: progressEmoji constant present");
    expect(contains(cpp, "QLatin1String(\"active\")"),
           "INV-2: filter == \"active\" branch present");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv3ShippedFilterSwitch) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "doneEmoji"),
           "INV-3: doneEmoji constant present");
    expect(contains(cpp, "QLatin1String(\"shipped\")"),
           "INV-3: filter == \"shipped\" branch present");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv4CaseInsensitive) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-4: status parsed with .toLower() canonicalisation.
    expect(contains(cpp, "ANTS-1247-INV-4"),
           "INV-4 anchor comment present");
    expect(contains(cpp, ".toString().toLower()"),
           "INV-4: status arg canonicalised via toLower()");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv5BadStatusError) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-5: unknown status yields bad_status code.
    expect(contains(cpp, "ANTS-1247-INV-5"),
           "INV-5 anchor comment present");
    expect(contains(cpp, "\"bad_status\""),
           "INV-5: code = bad_status emitted");
    expect(contains(cpp, "unknown status filter"),
           "INV-5: error message names the filter category");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv6CacheInvariantPreserved) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-6: fresh check preserved; filter doesn't invalidate.
    expect(contains(cpp, "ANTS-1247-INV-6"),
           "INV-6 anchor comment present");
    expect(contains(cpp, "kRoadmapCacheTtlMs"),
           "INV-6: TTL constant still drives the fresh check");
    expect(contains(cpp, "m_roadmapCacheMtimeMs"),
           "INV-6: mtime check still drives the fresh check");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv7FilterEcho) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1247-INV-7"),
           "INV-7 anchor comment present");
    expect(contains(cpp, "out[\"filter\"] = filter"),
           "INV-7: response assembly sets `filter` field");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv8McpInputSchema) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // ANTS-1247-INV-8: tools/list registration declares status enum.
    expect(contains(ci, "ANTS-1247-INV-8"),
           "INV-8 anchor comment present in claudeintegration.cpp");
    expect(contains(ci, "statusEnum.append(\"all\")"),
           "INV-8: inputSchema enum includes \"all\"");
    expect(contains(ci, "statusEnum.append(\"active\")"),
           "INV-8: inputSchema enum includes \"active\"");
    expect(contains(ci, "statusEnum.append(\"shipped\")"),
           "INV-8: inputSchema enum includes \"shipped\"");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv9McpDispatchExtractsStatus) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1247-INV-9"),
           "INV-9 anchor comment present in claudeintegration.cpp");
    expect(contains(ci, "args.value(\"status\")"),
           "INV-9: dispatch extracts arguments.status");
    expect(contains(ci, "statusVal.isString()"),
           "INV-9: dispatch gates on isString()");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv10CountPostFilter) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1247-INV-10"),
           "INV-10 anchor comment present");
    expect(contains(cpp, "out[\"count\"] = filtered.size()"),
           "INV-10: count uses filtered.size(), not cached.size()");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, Inv11ErrorMessageHygiene) {
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1247-INV-11"),
           "INV-11 anchor comment present");
    expect(contains(cpp, "verbatim.truncate(64)"),
           "INV-11: error echo capped at 64 bytes");
    expect(contains(cpp, "QChar('?')"),
           "INV-11: control-byte replacement uses '?' char");
    if (g_failures) FAIL();
}

TEST(mcp_roadmap_status_filter, ProviderLambdaWidened) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    // The MainWindow provider lambda must thread `status` through to
    // cmdRoadmapQuery(req).
    expect(contains(mw, "setRoadmapQueryProvider("),
           "MainWindow calls setRoadmapQueryProvider");
    expect(contains(mw, "[this](const QString &status)"),
           "Provider lambda accepts `const QString &status`");
    expect(contains(mw, "req[\"status\"] = status"),
           "Provider lambda forwards status to req[\"status\"]");
    expect(contains(mw, "cmdRoadmapQuery(req)"),
           "Provider lambda calls cmdRoadmapQuery with req");
    if (g_failures) FAIL();
}
