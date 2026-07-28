// Feature-conformance test for ANTS-1112 MCP wiring. Source-grep
// approach: read claudeintegration.cpp + mainwindow.cpp + remotecontrol.h/.cpp
// and verify all 5 tool names are registered in each layer.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_H_PATH
#error "SRC_REMOTECONTROL_H_PATH compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

namespace {


constexpr const char *kToolNames[6] = {
    "indie_review_partition",
    "indie_review_brief",
    "indie_review_corroborate",
    "indie_review_synthesis_prompt",
    "indie_review_fold_in",
    "indie_review_orchestrate",  // ANTS-1279
};

constexpr const char *kCmdMethods[6] = {
    "cmdIndieReviewPartition",
    "cmdIndieReviewBrief",
    "cmdIndieReviewCorroborate",
    "cmdIndieReviewSynthesisPrompt",
    "cmdIndieReviewFoldIn",
    "cmdIndieReviewOrchestrate",  // ANTS-1279
};

}  // namespace

TEST(McpIndieReviewTools, Inv9AllToolNamesInToolsList) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    for (const char *name : kToolNames) {
        const std::string nameQuoted = std::string("\"") + name + "\"";
        EXPECT_NE(ci.find(nameQuoted), std::string::npos)
            << "tool name " << name << " missing from claudeintegration.cpp";
    }
}

TEST(McpIndieReviewTools, AllProvidersRegisteredInMainWindow) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    for (const char *name : kToolNames) {
        const std::string call =
            std::string("registerToolProvider(\"") + name + "\"";
        EXPECT_NE(mw.find(call), std::string::npos)
            << "registerToolProvider(\"" << name
            << "\", ...) missing from mainwindow.cpp";
    }
}

TEST(McpIndieReviewTools, AllCmdMethodsDeclaredInHeader) {
    const std::string rch = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(rch.empty());
    for (const char *m : kCmdMethods) {
        EXPECT_NE(rch.find(m), std::string::npos)
            << "method " << m << " missing from remotecontrol.h";
    }
}

TEST(McpIndieReviewTools, AllCmdMethodsDefinedInCpp) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    for (const char *m : kCmdMethods) {
        const std::string defn = std::string("RemoteControl::") + m;
        EXPECT_NE(rc.find(defn), std::string::npos)
            << "definition RemoteControl::" << m
            << " missing from remotecontrol.cpp";
    }
}

// ANTS-1279 — the orchestrate handler composes the dispatch manifest from
// the existing engine functions and emits the report-collection contract.
TEST(McpIndieReviewTools, Ants1279OrchestrateComposesManifest) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto p = rc.find("RemoteControl::cmdIndieReviewOrchestrate");
    ASSERT_NE(p, std::string::npos);
    // ANTS-3481 widened 3500→4800: the module_map_unparseable branch added
    // ~1 KB to the handler head, pushing the later literals past the window.
    const std::string body = rc.substr(p, 4800);
    EXPECT_NE(body.find("derivePartition"), std::string::npos)
        << "orchestrate must derive the partition";
    EXPECT_NE(body.find("assembleBriefManifest"), std::string::npos)
        << "orchestrate must assemble per-lane brief manifests";
    EXPECT_NE(body.find("suggestedMerges"), std::string::npos)
        << "orchestrate must surface suggested_merges (ANTS-1288 reuse)";
    EXPECT_NE(body.find("reports_dir"), std::string::npos)
        << "orchestrate must emit a reports_dir for the collect phase";
    EXPECT_NE(body.find("report_path"), std::string::npos)
        << "orchestrate must emit a per-lane report_path";
    EXPECT_NE(body.find("next_steps"), std::string::npos)
        << "orchestrate must emit next_steps wiring to corroborate/fold_in";
    EXPECT_NE(body.find("include_briefs"), std::string::npos)
        << "orchestrate must honour the include_briefs toggle";
}

// ANTS-1288 — the partition handler emits suggested_merges, computed via
// the engine helper (locks the wiring against accidental removal).
TEST(McpIndieReviewTools, Ants1288PartitionEmitsSuggestedMerges) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto p = rc.find("RemoteControl::cmdIndieReviewPartition");
    ASSERT_NE(p, std::string::npos);
    // Window widened 2000 → 4000 by ANTS-3709: the computed-partition
    // fallback grew the handler, pushing suggested_merges past the old
    // scrape window. A fixed-byte window measures handler length, not the
    // wiring it claims to lock.
    const std::string body = rc.substr(p, 4000);
    EXPECT_NE(body.find("suggested_merges"), std::string::npos)
        << "cmdIndieReviewPartition no longer emits suggested_merges";
    EXPECT_NE(body.find("IndieReviewEngine::suggestedMerges"),
              std::string::npos)
        << "cmdIndieReviewPartition no longer calls the engine helper";
}

// ANTS-3375 / ANTS-3493 INV-11 — cmdIndieReviewBrief synthesises an
// ad-hoc lane from a caller-supplied `source_paths[]` when the lane is
// absent from the derived partition (mirrors cold_eyes_brief's ANTS-1508
// doc_paths[] fallback). Source-grep over the handler body.
TEST(McpIndieReviewTools, Ants3375SourcePathsAdHocLaneInHandler) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find("RemoteControl::cmdIndieReviewBrief");
    ASSERT_NE(pos, std::string::npos);
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    EXPECT_NE(body.find("\"source_paths\""), std::string::npos)
        << "INV-11: handler must read the source_paths field";
    EXPECT_NE(body.find("PathValidation::validatePath"), std::string::npos)
        << "INV-11: source_paths entries must route through the "
           "traversal-guard chokepoint";
    EXPECT_NE(body.find("assembleBriefManifest"), std::string::npos)
        << "INV-11: handler must feed the ad-hoc lane to "
           "assembleBriefManifest";
}

// ANTS-3375 / ANTS-3493 INV-12 — the unknown-lane refusal names the
// source_paths[] override and carries known_lanes + source_paths_rejected
// so the caller recovers without a second partition round-trip.
TEST(McpIndieReviewTools, Ants3375NotFoundNamesSourcePathsOverride) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(rc.empty());
    const auto pos = rc.find("RemoteControl::cmdIndieReviewBrief");
    ASSERT_NE(pos, std::string::npos);
    const auto end = rc.find("\n}\n", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = rc.substr(pos, end - pos);

    EXPECT_NE(body.find("source_paths[] override"), std::string::npos)
        << "INV-12: not_found refusal must name the source_paths[] override";
    EXPECT_NE(body.find("known_lanes"), std::string::npos)
        << "INV-12: refusal must list known_lanes for recovery";
    EXPECT_NE(body.find("source_paths_rejected"), std::string::npos)
        << "INV-12: refusal must surface per-path reject reasons";
}

// ANTS-3375 / ANTS-3493 INV-13 — the indie_review_brief descriptor
// declares the optional source_paths array prop and cites the roadmap
// IDs so the ad-hoc mode is discoverable from tools/list.
TEST(McpIndieReviewTools, Ants3375SourcePathsSchemaDeclared) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto pos = ci.find("t[\"name\"] = \"indie_review_brief\"");
    ASSERT_NE(pos, std::string::npos);
    const auto end = ci.find("tools.append(t);", pos);
    ASSERT_NE(end, std::string::npos);
    const std::string region = ci.substr(pos, end - pos);

    EXPECT_NE(region.find("props[\"source_paths\"]"), std::string::npos)
        << "INV-13: source_paths prop not declared on indie_review_brief";
    EXPECT_NE(region.find("ANTS-3375"), std::string::npos)
        << "INV-13: descriptor must cite ANTS-3375 for discoverability";
}

// ANTS-3713 — indie_review_corroborate accepts an absolute reports_dir under
// allow_outside_project, reusing test_audit_synthesis_prompt's opt-in name
// (ANTS-1455) rather than inventing a second one. Scoped to this verb's own
// descriptor via mcpToolDescriptor, because the sibling verb declares the
// same property and a whole-file grep would false-green.
TEST(McpIndieReviewTools, Ants3713CorroborateAllowsOutsideProject) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string desc =
        ants_test::mcpToolDescriptor(ci, "indie_review_corroborate");
    ASSERT_FALSE(desc.empty())
        << "indie_review_corroborate descriptor not found in the tools list";
    EXPECT_NE(desc.find("props[\"allow_outside_project\"]"), std::string::npos)
        << "allow_outside_project is not declared on this verb's schema, so "
           "additionalProperties:false rejects it";

    // Handler side: the flag relaxes the anchor AND routes to the
    // already-anchored engine entry point, so ANTS-1282 INV-3 still guards
    // the default path.
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    EXPECT_NE(rc.find("/*allowOutsideRoot=*/allowOutside"), std::string::npos);
    EXPECT_NE(rc.find("corroboratedFindingsFromCanonicalDir"),
              std::string::npos);
}

// INV-14 (ANTS-1581 reversal) — the blanket "the skill does not call this
// tool" note is gone from BOTH families, and the one surviving warning is
// scoped to indie_review_dispatch, whose difference is the reviewer it runs
// on rather than who calls it.
TEST(McpIndieReviewTools, Inv14ParallelApiNoteReversedToDispatchOnly) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    // The old note steered readers away from every cold_eyes_ /
    // indie_review_ verb — the opposite of the standing rule that the MCP
    // verbs are the default path. No trace of it may remain, or some verbs
    // still carry it and the reversal is half-applied.
    EXPECT_EQ(ci.find("Parallel API:"), std::string::npos)
        << "INV-14: the ANTS-1581(b) 'Parallel API' note must be gone, not "
           "narrowed further";
    // The surviving carve-out, and its guard.
    const auto note = ci.find("Weaker reviewer:");
    ASSERT_NE(note, std::string::npos)
        << "INV-14: indie_review_dispatch keeps a note saying WHY it differs";
    const auto guard =
        ci.find("name == QLatin1String(\"indie_review_dispatch\")");
    ASSERT_NE(guard, std::string::npos)
        << "INV-14: the note is name-scoped, not prefix-matched over a family";
    EXPECT_LT(guard, note)
        << "INV-14: the guard must precede the note it gates";
}

// INV-15 (ANTS-1581 reversal) — the catalog hint is the other place a caller
// picks a verb from, so it must not present the local-endpoint verb as the
// entry point for a review.
TEST(McpIndieReviewTools, Inv15DispatchSelectionHintNamesTheWeakerReviewer) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const std::string hint =
        ants_test::mcpToolDescriptor(ci, "indie_review_dispatch");
    ASSERT_FALSE(hint.empty());
    EXPECT_NE(hint.find("LOCAL AI endpoint"), std::string::npos)
        << "INV-15: the hint must say where the review actually runs";
    EXPECT_EQ(hint.find("entry-point orchestrator"), std::string::npos)
        << "INV-15: dispatch is not the entry point for a Claude review";
}

TEST(McpIndieReviewTools, AllSchemasUseAdditionalPropertiesFalse) {
    // Defensive: every new tool's inputSchema sets additionalProperties=false
    // so unknown keys are rejected. Region scoped to JUST the indie_review_*
    // block — start at indie_review_partition, end at the next non-indie
    // tool block (currently debt_sweep_scan, ANTS-1113). ANTS-1352 added
    // indie_review_dispatch within this region (sixth tool).
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    const auto block_start = ci.find("\"indie_review_partition\"");
    ASSERT_NE(block_start, std::string::npos);
    // End: the next ANTS-NNNN comment after fold_in marks the next
    // tool series. Falls back to `result["tools"] = tools;` when no
    // subsequent block exists.
    const auto fold_in_pos = ci.find("\"indie_review_fold_in\"", block_start);
    ASSERT_NE(fold_in_pos, std::string::npos);
    auto block_end = ci.find("// ANTS-1113", fold_in_pos);
    if (block_end == std::string::npos) {
        block_end = ci.find("result[\"tools\"] = tools;", fold_in_pos);
    }
    ASSERT_NE(block_end, std::string::npos);
    const std::string region = ci.substr(block_start, block_end - block_start);
    int count = 0;
    size_t pos = 0;
    while ((pos = region.find("additionalProperties", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    // ANTS-2068 — floor, not exact: every indie_review_* tool schema must
    // pin additionalProperties:false, so adding a tool shouldn't false-fail
    // this; a drop below the known floor means one was loosened/removed.
    EXPECT_GE(count, 7)
        << "expected >= 7 additionalProperties=false in the indie_review "
           "tool block (5 original indie_review_* tools + ANTS-1352 "
           "indie_review_dispatch + ANTS-1279 indie_review_orchestrate); "
           "fewer means a schema dropped its additionalProperties guard";
}
