// Feature-conformance test for ANTS-1319 — cold_eyes_* MCP wiring.
// See tests/features/mcp_cold_eyes/spec.md.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Region: `// ANTS-1319` cold-eyes registration block end.
size_t coldEyesBlockEnd(const std::string &ci, size_t start) {
    return ci.find("// ANTS-1284 — hoist", start);
}

}  // namespace

// REG-1
TEST(McpColdEyes, FourToolNamesRegisteredWithAnchor) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1319");
    ASSERT_NE(pos, std::string::npos)
        << "// ANTS-1319 anchor missing from claudeintegration.cpp";
    const auto end = coldEyesBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);
    for (const std::string &name : {"cold_eyes_partition",
                                    "cold_eyes_brief",
                                    "cold_eyes_cross_doc_diff",
                                    "cold_eyes_fold_in"}) {
        EXPECT_NE(region.find("t[\"name\"] = \"" + name + "\""),
                  std::string::npos)
            << name << " registration missing under ANTS-1319 anchor";
    }
}

// REG-2
TEST(McpColdEyes, SchemaRequiredArraysMatchInv10) {
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1319");
    ASSERT_NE(pos, std::string::npos);
    const auto end = coldEyesBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);

    // brief / fold_in each call `req.append("…")` for their one
    // required arg.
    EXPECT_NE(region.find("req.append(\"lane\")"), std::string::npos)
        << "cold_eyes_brief should require lane";
    EXPECT_NE(region.find("req.append(\"actionable\")"), std::string::npos)
        << "cold_eyes_fold_in should require actionable";

    // partition: no `required` (scope is optional). Negative check —
    // look for the partition block specifically. Its block runs from
    // `t["name"] = "cold_eyes_partition"` to the next `tools.append(t);`.
    const auto pPart = region.find("t[\"name\"] = \"cold_eyes_partition\"");
    ASSERT_NE(pPart, std::string::npos);
    const auto pEnd  = region.find("tools.append(t);", pPart);
    ASSERT_NE(pEnd, std::string::npos);
    const std::string partRegion = region.substr(pPart, pEnd - pPart);
    EXPECT_EQ(partRegion.find("schema[\"required\"]"), std::string::npos)
        << "cold_eyes_partition unexpectedly sets a required array";

    // ANTS-1509 — cross_doc_diff is XOR `reports`/`reports_dir`,
    // enforced at the handler (cmdColdEyesCrossDocDiff). The schema
    // must NOT set required (mirrors indie_review_corroborate's
    // ANTS-1282 INV-1 pattern). Negative check on the block.
    const auto pDiff = region.find(
        "t[\"name\"] = \"cold_eyes_cross_doc_diff\"");
    ASSERT_NE(pDiff, std::string::npos);
    const auto pDiffEnd = region.find("tools.append(t);", pDiff);
    ASSERT_NE(pDiffEnd, std::string::npos);
    const std::string diffRegion = region.substr(pDiff, pDiffEnd - pDiff);
    EXPECT_EQ(diffRegion.find("schema[\"required\"]"), std::string::npos)
        << "cold_eyes_cross_doc_diff unexpectedly sets a required array; "
           "XOR is enforced at the handler per ANTS-1509";
    EXPECT_NE(diffRegion.find("props[\"reports\"]"), std::string::npos)
        << "cross_doc_diff missing inline reports prop (ANTS-1509)";
    EXPECT_NE(diffRegion.find("props[\"reports_dir\"]"), std::string::npos)
        << "cross_doc_diff missing reports_dir prop";
}

// REG-3
TEST(McpColdEyes, CmdColdEyesExtractsAllArgs) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"scope\")).toString()"),
              std::string::npos)
        << "scope arg not extracted in cmdColdEyesPartition";
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"lane\")).toString()"),
              std::string::npos)
        << "lane arg not extracted in cmdColdEyesBrief";
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"reports_dir\"))"),
              std::string::npos)
        << "reports_dir arg not extracted in cmdColdEyesCrossDocDiff";
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"actionable\")).toArray()"),
              std::string::npos)
        << "actionable arg not extracted in cmdColdEyesFoldIn";
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"date_iso\")).toString()"),
              std::string::npos)
        << "date_iso arg not extracted in cmdColdEyesFoldIn";
}

// REG-4
TEST(McpColdEyes, BadScopeErrorCodePresent) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("\"bad_scope\""), std::string::npos)
        << "bad_scope error code not emitted on unknown scope arg";
}

// REG-5
TEST(McpColdEyes, EchoHygieneMatchesInv11) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    // ANTS-1319 cold-eyes section uses a shared helper `ceSanitiseEcho`
    // that internally applies `verbatim.truncate(64)` + `< 0x20` substitution.
    const auto pos = rc.find("ceSanitiseEcho");
    ASSERT_NE(pos, std::string::npos)
        << "ceSanitiseEcho helper not declared in cold-eyes section";
    // Verify the helper body itself carries both INV-11 markers.
    const auto helperPos = rc.find("QString ceSanitiseEcho");
    ASSERT_NE(helperPos, std::string::npos);
    const auto helperEnd = rc.find("}\n", helperPos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = rc.substr(helperPos, helperEnd - helperPos);
    EXPECT_NE(body.find("verbatim.truncate(64)"), std::string::npos)
        << "echo not capped at 64 bytes";
    EXPECT_NE(body.find("verbatim.at(i).unicode() < 0x20"), std::string::npos)
        << "control-char substitution missing";
}

// REG-6
TEST(McpColdEyes, CacheMembersDeclaredInHeader) {
    const std::string rh = slurp(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rh.empty());
    EXPECT_NE(rh.find("m_coldEyesCache"), std::string::npos)
        << "m_coldEyesCache member missing from remotecontrol.h";
    EXPECT_NE(rh.find("m_coldEyesCacheStampMs"), std::string::npos)
        << "m_coldEyesCacheStampMs member missing";
    EXPECT_NE(rh.find("kColdEyesCacheTtlMs"), std::string::npos)
        << "kColdEyesCacheTtlMs constant missing";
    EXPECT_NE(rh.find("ColdEyesEngine::PartitionResult"), std::string::npos)
        << "PartitionResult type-signature on cache member missing";
}

// REG-7
TEST(McpColdEyes, ProviderLambdasForwardArgs) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    for (const std::string &cmd : {"cmdColdEyesPartition",
                                   "cmdColdEyesBrief",
                                   "cmdColdEyesCrossDocDiff",
                                   "cmdColdEyesFoldIn"}) {
        EXPECT_NE(mw.find(cmd + "(args)"), std::string::npos)
            << cmd << " not wired in mainwindow.cpp";
    }
    EXPECT_NE(mw.find("registerToolProvider(\"cold_eyes_partition\""),
              std::string::npos);
    EXPECT_NE(mw.find("registerToolProvider(\"cold_eyes_fold_in\""),
              std::string::npos);
}

// ANTS-1634 INV-1 + INV-2 — sparse_partition_hint mentions both
// escape hatches: ANTS-1508 (lane-agnostic cold_eyes_brief) and
// ANTS-1412 (`.cold-eyes/partition.json` override). The hint is the
// caller's first inroad when a default-scope partition comes back
// near-empty; surfacing both workarounds inline prevents the
// "give up and skip the verb" pattern observed in cross-session reports.
TEST(McpColdEyes, Ants1634SparsePartitionHintMentionsBriefAndOverride) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find("sparse_partition_hint");
    ASSERT_NE(pos, std::string::npos)
        << "sparse_partition_hint emission missing from remotecontrol.cpp";
    // Scope the grep to a window of ~600 bytes after the field name —
    // bounds the literal lookup to the hint string body and avoids
    // matching unrelated occurrences elsewhere in the file.
    const std::string window = rc.substr(pos, 600);
    EXPECT_NE(window.find("cold_eyes_brief"), std::string::npos)
        << "INV-1: hint should point at cold_eyes_brief";
    EXPECT_NE(window.find("ANTS-1508"), std::string::npos)
        << "INV-1: hint should cite ANTS-1508";
    EXPECT_NE(window.find(".cold-eyes/partition.json"), std::string::npos)
        << "INV-2: hint should point at the override file";
    EXPECT_NE(window.find("ANTS-1412"), std::string::npos)
        << "INV-2: hint should cite ANTS-1412";
}

// REG-8
TEST(McpColdEyes, FoldInUsesRoadmapFoldInAllocateAndInsert) {
    const std::string rc = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    // Find the cmdColdEyesFoldIn body and assert both calls appear
    // within it.
    const auto pos = rc.find("QJsonDocument RemoteControl::cmdColdEyesFoldIn");
    ASSERT_NE(pos, std::string::npos);
    // Scan to the next top-level closing brace.
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);
    EXPECT_NE(body.find("RoadmapFoldIn::allocateIds"), std::string::npos)
        << "INV-6: cmdColdEyesFoldIn must call RoadmapFoldIn::allocateIds";
    EXPECT_NE(body.find("RoadmapFoldIn::insertBlock"), std::string::npos)
        << "INV-6: cmdColdEyesFoldIn must call RoadmapFoldIn::insertBlock";
    EXPECT_NE(body.find("ColdEyesEngine::templateColdEyesFoldInBlock"),
              std::string::npos)
        << "cmdColdEyesFoldIn must call the engine's template helper";
}
