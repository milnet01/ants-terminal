// Feature-conformance test for ANTS-1319 — cold_eyes_* MCP wiring.
// See tests/features/mcp_cold_eyes/spec.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef ANTS_MAINWINDOW_SOURCES
#error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif

namespace {


// Region: `// ANTS-1319` cold-eyes registration block end.
size_t coldEyesBlockEnd(const std::string &ci, size_t start) {
    return ci.find("// ANTS-1284 — hoist", start);
}

}  // namespace

// REG-1
TEST(McpColdEyes, ToolNamesRegisteredWithAnchor) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1319");
    ASSERT_NE(pos, std::string::npos)
        << "// ANTS-1319 anchor missing from claudeintegration.cpp";
    const auto end = coldEyesBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);
    for (const std::string name : {"cold_eyes_partition",
                                   "cold_eyes_brief"}) {
        EXPECT_NE(region.find("t[\"name\"] = \"" + name + "\""),
                  std::string::npos)
            << name << " registration missing under ANTS-1319 anchor";
    }
}

// REG-2
TEST(McpColdEyes, SchemaRequiredArraysMatchInv10) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("// ANTS-1319");
    ASSERT_NE(pos, std::string::npos);
    const auto end = coldEyesBlockEnd(ci, pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);

    // brief calls `req.append("lane")` for its one required arg.
    EXPECT_NE(region.find("req.append(\"lane\")"), std::string::npos)
        << "cold_eyes_brief should require lane";
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
}

// REG-3
TEST(McpColdEyes, CmdColdEyesExtractsAllArgs) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"scope\")).toString()"),
              std::string::npos)
        << "scope arg not extracted in cmdColdEyesPartition";
    EXPECT_NE(rc.find("req.value(QStringLiteral(\"lane\")).toString()"),
              std::string::npos)
        << "lane arg not extracted in cmdColdEyesBrief";
}

// REG-4
TEST(McpColdEyes, BadScopeErrorCodePresent) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_NE(rc.find("\"bad_scope\""), std::string::npos)
        << "bad_scope error code not emitted on unknown scope arg";
}

// REG-5
TEST(McpColdEyes, EchoHygieneMatchesInv11) {
    const std::string rc = ants_test::slurpRemoteControl();
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
    const std::string rh = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
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
    const std::string mw = ants_test::slurpMainWindow();
    ASSERT_FALSE(mw.empty());
    // ANTS-1782 — these are pure RC-delegate shims registered via the
    // rcDelegate(rc, &RemoteControl::cmd*) factory, which forwards `args`
    // wholesale. Assert the verb reference rather than the old inline
    // `cmd(args)` call shape.
    for (const std::string cmd : {"cmdColdEyesPartition",
                                  "cmdColdEyesBrief"}) {
        EXPECT_NE(mw.find(cmd), std::string::npos)
            << cmd << " not wired in mainwindow.cpp";
    }
    EXPECT_NE(mw.find("registerToolProvider(\"cold_eyes_partition\""),
              std::string::npos);
    EXPECT_NE(mw.find("registerToolProvider(\"cold_eyes_brief\""),
              std::string::npos);
}

// ANTS-1634 INV-1 + INV-2 — sparse_partition_hint mentions both
// escape hatches: ANTS-1508 (lane-agnostic cold_eyes_brief) and
// ANTS-1412 (`.cold-eyes/partition.json` override). The hint is the
// caller's first inroad when a default-scope partition comes back
// near-empty; surfacing both workarounds inline prevents the
// "give up and skip the verb" pattern observed in cross-session reports.
TEST(McpColdEyes, Ants1634SparsePartitionHintMentionsBriefAndOverride) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    // ANTS-3567 added a second sparse_partition_hint in cmdIndieReviewPartition
    // (earlier in the file), so scope the lookup to cmdColdEyesPartition's body
    // rather than the global first occurrence.
    const auto fn = rc.find("RemoteControl::cmdColdEyesPartition");
    ASSERT_NE(fn, std::string::npos)
        << "cmdColdEyesPartition body not found in remotecontrol.cpp";
    const auto pos = rc.find("sparse_partition_hint", fn);
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


// ANTS-1634(b) INV-11 — claudeintegration.cpp's cold_eyes_brief
// registration block declares the `prior_loop_fixes` array prop with
// `{title, summary}` item objects. Schema-shape source-grep so a
// future rename / signature drift is caught at test time, before any
// orchestrator that depends on the field hits an `unknown_arg`-shape
// refusal.
TEST(McpColdEyes, Ants1634PriorLoopFixesSchemaDeclared) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    // Scope to the cold_eyes_brief block: starts at the name literal,
    // ends at the next `tools.append(t);`.
    const auto pos = ci.find("t[\"name\"] = \"cold_eyes_brief\"");
    ASSERT_NE(pos, std::string::npos);
    const auto end = ci.find("tools.append(t);", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);

    EXPECT_NE(region.find("props[\"prior_loop_fixes\"]"), std::string::npos)
        << "INV-11: prior_loop_fixes prop not added to cold_eyes_brief schema";
    EXPECT_NE(region.find("priorFixesItemProps[\"title\"]"),
              std::string::npos)
        << "INV-11: item schema missing `title` property";
    EXPECT_NE(region.find("priorFixesItemProps[\"summary\"]"),
              std::string::npos)
        << "INV-11: item schema missing `summary` property";
}

// ANTS-1634(b) INV-12 — cmdColdEyesBrief extracts the
// `prior_loop_fixes` array from the request and forwards it to
// ColdEyesEngine::assembleBriefManifest as a third arg. Source-grep
// only — the wiring's behavioural correctness is covered by the
// engine tests under `cold_eyes_engine`.
TEST(McpColdEyes, Ants1634PriorLoopFixesExtractedInHandler) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find(
        "QJsonDocument RemoteControl::cmdColdEyesBrief");
    ASSERT_NE(pos, std::string::npos);
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    EXPECT_NE(body.find("\"prior_loop_fixes\""), std::string::npos)
        << "INV-12: cmdColdEyesBrief must read the prior_loop_fixes field";
    EXPECT_NE(body.find("ColdEyesEngine::PriorLoopFix"), std::string::npos)
        << "INV-12: handler must construct engine PriorLoopFix records";
    // Whitespace-tolerant: assert the engine call and the priorFixes
    // identifier both appear in the handler body. A formatter that
    // breaks the call across more lines must still pass this gate.
    EXPECT_NE(body.find("assembleBriefManifest"), std::string::npos)
        << "INV-12: handler must call assembleBriefManifest";
    EXPECT_NE(body.find("priorFixes"), std::string::npos)
        << "INV-12: handler must forward priorFixes";
}
