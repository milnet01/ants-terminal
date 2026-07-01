// ANTS-1247 — feature-conformance test for the `status` filter on
// roadmap_query (MCP tool + IPC verb). Source-grep harness; no GUI,
// no QTemporaryDir.
//
// Each INV maps to a // ANTS-1247-INV-N anchor in source.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {




bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int countOccurrences(const std::string &hay, const std::string &needle) {
    int n = 0;
    for (std::string::size_type p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

}  // namespace

TEST(mcp_roadmap_status_filter, Inv1BackCompatSignatureDefaulted) {
    expect_reset();
    const std::string h = ants_test::slurpFile(SRC_RC_HEADER);
    // ANTS-1247-INV-1: signature accepts QJsonObject with default {}.
    expect(contains(h, "cmdRoadmapQuery(const QJsonObject &req = {})"),
           "INV-1: cmdRoadmapQuery has `const QJsonObject &req = {}` default param");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv2ActiveFilterSwitch) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-2: planned + in-progress emoji compare.
    expect(contains(cpp, "ANTS-1247-INV-2"),
           "INV-2 anchor comment present in remotecontrol.cpp");
    expect(contains(cpp, "plannedEmoji"),
           "INV-2: plannedEmoji constant present");
    expect(contains(cpp, "progressEmoji"),
           "INV-2: progressEmoji constant present");
    expect(contains(cpp, "QLatin1String(\"active\")"),
           "INV-2: filter == \"active\" branch present");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv3ShippedFilterSwitch) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "doneEmoji"),
           "INV-3: doneEmoji constant present");
    expect(contains(cpp, "QLatin1String(\"shipped\")"),
           "INV-3: filter == \"shipped\" branch present");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv4CaseInsensitive) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-4: status parsed with .toLower() canonicalisation.
    expect(contains(cpp, "ANTS-1247-INV-4"),
           "INV-4 anchor comment present");
    expect(contains(cpp, ".toString().toLower()"),
           "INV-4: status arg canonicalised via toLower()");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv5BadStatusError) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-5: unknown status yields bad_status code.
    expect(contains(cpp, "ANTS-1247-INV-5"),
           "INV-5 anchor comment present");
    expect(contains(cpp, "\"bad_status\""),
           "INV-5: code = bad_status emitted");
    expect(contains(cpp, "unknown status filter"),
           "INV-5: error message names the filter category");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv6CacheInvariantPreserved) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // ANTS-1247-INV-6: fresh check preserved; filter doesn't invalidate.
    expect(contains(cpp, "ANTS-1247-INV-6"),
           "INV-6 anchor comment present");
    expect(contains(cpp, "kRoadmapCacheTtlMs"),
           "INV-6: TTL constant still drives the fresh check");
    expect(contains(cpp, "m_roadmapCacheMtimeMs"),
           "INV-6: mtime check still drives the fresh check");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv7FilterEcho) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1247-INV-7"),
           "INV-7 anchor comment present");
    expect(contains(cpp, "out[\"filter\"] = filter"),
           "INV-7: response assembly sets `filter` field");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv8McpInputSchema) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // ANTS-1247-INV-8: tools/list registration declares status enum.
    expect(contains(ci, "ANTS-1247-INV-8"),
           "INV-8 anchor comment present in claudeintegration.cpp");
    expect(contains(ci, "statusEnum.append(\"all\")"),
           "INV-8: inputSchema enum includes \"all\"");
    expect(contains(ci, "statusEnum.append(\"active\")"),
           "INV-8: inputSchema enum includes \"active\"");
    expect(contains(ci, "statusEnum.append(\"shipped\")"),
           "INV-8: inputSchema enum includes \"shipped\"");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3400 — the read verb accepts roadmap_log's lifecycle vocabulary
// (planned / in-progress / considered) as first-class status filters and
// echoes the accepted set on refusal.
TEST(mcp_roadmap_status_filter, Ants3400GranularLifecycleFilters) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-3400"),
           "ANTS-3400 anchor comment present");
    expect(contains(cpp, "kAcceptedStatusFilters"),
           "ANTS-3400: accepted-status list present");
    expect(contains(cpp, "out[\"accepted\"]"),
           "ANTS-3400: bad_status envelope echoes the accepted set");
    expect(contains(cpp, "QLatin1String(\"in-progress\")"),
           "ANTS-3400: in-progress granular filter branch present");
    expect(contains(cpp, "consideredEmoji"),
           "ANTS-3400: considered (💭) filter emoji present");
    expect(contains(cpp, "sectionFilter"),
           "ANTS-3400: section_index collapses granular names to aggregate");
    // Schema surfaces the granular enum values + max_body_bytes (ANTS-3402).
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "statusEnum.append(\"planned\")"),
           "ANTS-3400: inputSchema enum includes \"planned\"");
    expect(contains(ci, "statusEnum.append(\"considered\")"),
           "ANTS-3400: inputSchema enum includes \"considered\"");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3408 — the granular planned/in-progress/considered arms must live
// in BOTH emission branches (section= and full-file/no-section). ANTS-3400
// added them to the section branch only; the full-file branch iterating
// m_roadmapCacheBullets silently returned 0 for a granular filter without a
// `section` arg (Contact_List feedback 2026-07-01). A whole-file `contains`
// check can't tell the two branches apart, so this is count-based: the
// planned arm literal must appear at least twice.
TEST(mcp_roadmap_status_filter, Ants3408GranularFiltersInBothBranches) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    // Both the section= and full-file predicates must carry the planned arm.
    expect(countOccurrences(
               cpp, "QLatin1String(\"planned\")     && (s == plannedEmoji)") >= 2,
           "ANTS-3408: planned granular arm present in both emission branches");
    expect(countOccurrences(
               cpp, "QLatin1String(\"in-progress\") && (s == progressEmoji)") >= 2,
           "ANTS-3408: in-progress granular arm present in both branches");
    expect(countOccurrences(
               cpp, "QLatin1String(\"considered\")  && (s == consideredEmoji)") >= 2,
           "ANTS-3408: considered granular arm present in both branches");
    expect(contains(cpp, "ANTS-3408"),
           "ANTS-3408 anchor comment present");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3402 — targeted id/ids fetch may raise the body cap via
// max_body_bytes; list emission stays at the 2000 cap.
TEST(mcp_roadmap_status_filter, Ants3402TargetedBodyCap) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "kRoadmapQueryBodyStoreCap"),
           "ANTS-3402: enlarged cache store cap present");
    expect(contains(cpp, "rcCapBodyFields"),
           "ANTS-3402: emission-time re-truncation helper present");
    expect(contains(cpp, "idBodyCap"),
           "ANTS-3402: id/ids path honours max_body_bytes");
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "max_body_bytes"),
           "ANTS-3402: max_body_bytes schema property declared");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv9McpDispatchExtractsStatus) {
    expect_reset();
    // Post-ANTS-1253: the per-tool dispatch extraction moved from
    // claudeintegration.cpp into the registerToolProvider lambda
    // body in mainwindow.cpp. INV-9 is now asserted there.
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "registerToolProvider(\"roadmap_query\""),
           "ANTS-1253: roadmap_query registered via registerToolProvider");
    expect(contains(mw, "args.value(\"status\")"),
           "INV-9: roadmap_query lambda extracts args.status");
    expect(contains(mw, "statusVal.isString()"),
           "INV-9: roadmap_query lambda gates on isString()");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv10CountPostFilter) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1247-INV-10"),
           "INV-10 anchor comment present");
    // ANTS-1436 — count is now post-PAGINATION (page.slice.size()),
    // which equals filtered.size() when no pagination applies.
    // INV-10's intent ("count reflects what was actually emitted in
    // bullets[], not the cache size") is preserved; the literal
    // changed from `filtered.size()` to `page.slice.size()`.
    expect(contains(cpp, "out[\"count\"] = page.slice.size()"),
           "INV-10: count uses page.slice.size() (post-pagination), "
           "not cached.size()");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, Inv11ErrorMessageHygiene) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-1247-INV-11"),
           "INV-11 anchor comment present");
    expect(contains(cpp, "verbatim.truncate(64)"),
           "INV-11: error echo capped at 64 bytes");
    expect(contains(cpp, "QChar('?')"),
           "INV-11: control-byte replacement uses '?' char");
    EXPECT_EQ(0, expect_failures());
}

TEST(mcp_roadmap_status_filter, ProviderLambdaWidened) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    // The roadmap_query lambda registered via registerToolProvider
    // (ANTS-1253) must thread `status` through to cmdRoadmapQuery(req).
    expect(contains(mw, "registerToolProvider(\"roadmap_query\""),
           "MainWindow registers roadmap_query (ANTS-1253)");
    expect(contains(mw, "args.value(\"status\")"),
           "Provider lambda extracts args.status (ANTS-1253 widened sig)");
    expect(contains(mw, "req[\"status\"] = status"),
           "Provider lambda forwards status to req[\"status\"]");
    expect(contains(mw, "cmdRoadmapQuery(req)"),
           "Provider lambda calls cmdRoadmapQuery with req");
    EXPECT_EQ(0, expect_failures());
}
