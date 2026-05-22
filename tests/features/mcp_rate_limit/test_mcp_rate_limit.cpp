// ANTS-1356 — feature-conformance test for per-tool MCP rate-limit.
// Pure-function tests against ClaudeIntegration::rateLimitCheck with
// synthetic nowMs + source-greps against claudeintegration.cpp for
// dispatcher-wiring invariants.

#include "../../_support/srcgrep.h"

#include "claudeintegration.h"

#include <gtest/gtest.h>

#include <QJsonObject>
#include <QString>
#include <QStringLiteral>

namespace {

const QString kCwdA = QStringLiteral("/proj/a");
const QString kCwdB = QStringLiteral("/proj/b");

class RateLimitTestFixture : public ::testing::Test {
protected:
    void TearDown() override {
        // Defensive: a test that leaves the override active would
        // poison its neighbours. Always reset.
        ClaudeIntegration::resetRateLimitCapsForTest();
    }
};

}  // namespace

// INV-1 — under-cap accepts.
TEST_F(RateLimitTestFixture, Inv1UnderCapAccepts) {
    ClaudeIntegration ci;
    EXPECT_EQ(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                kCwdA, /*nowMs=*/1000), 0);
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(
        QStringLiteral("roadmap_query"), kCwdA), 1);
}

// INV-2 — at-cap refuses with positive retry_after_ms.
TEST_F(RateLimitTestFixture, Inv2AtCapRefuses) {
    ClaudeIntegration ci;
    // Drive the Expensive tier (10/min) for a tight loop.
    const QString tool = QStringLiteral("audit_run");
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000 + i), 0)
            << "call " << i << " of 10 should accept";
    }
    // 11th call at nowMs=1010 should refuse with retry_after_ms =
    // (1000 + 60000) - 1010 = 59990.
    const qint64 retry = ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1010);
    EXPECT_GT(retry, 0);
    EXPECT_EQ(retry, qint64{59990});
}

// INV-3 — post-window allowed again.
TEST_F(RateLimitTestFixture, Inv3PostWindowAccepts) {
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("audit_run");
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000 + i), 0);
    }
    // Refused at nowMs=1010.
    EXPECT_GT(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1010), 0);
    // Jump past the window — at nowMs=61'010 the prune threshold is
    // 1010, which drops every entry (ts < 1010 for all 10). The
    // bucket empties; INV-6 reset path fires; fresh entry inserted.
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/61'010), 0);
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(tool, kCwdA), 1);
}

// INV-4 — ControlPlane never refuses + map not grown.
TEST_F(RateLimitTestFixture, Inv4ControlPlaneUncapped) {
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("get_session_info");
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000 + i), 0)
            << "ControlPlane call " << i << " should never refuse";
    }
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 0)
        << "ControlPlane tools must not populate m_rateLimitBuckets";
}

// INV-5 — cache-hit gate. Source-grep proves rateLimitCheck runs
// before the cache lookup; behavioural side confirmed by the cap
// firing on a cacheable tool (last_audit_summary is Cheap + cached).
TEST_F(RateLimitTestFixture, Inv5CacheHitsConsume) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());

    const auto dispatchStart = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(dispatchStart, std::string::npos);
    // Bound the search to the dispatcher region for tools/call.
    // Window widened to 16000 (ANTS-1415 Phase 3b added the
    // TabSpecific refusal branch, pushing the cache gate past the
    // original 9000-char window).
    const std::string region = cc.substr(dispatchStart, 16000);

    const auto rateLimitPos = region.find("rateLimitCheck(toolName");
    const auto cachePos     = region.find("isIdempotentReadTool(toolName)");
    ASSERT_NE(rateLimitPos, std::string::npos);
    ASSERT_NE(cachePos, std::string::npos);
    EXPECT_LT(rateLimitPos, cachePos)
        << "INV-5: rateLimitCheck must run before the cache lookup; "
           "found rateLimitCheck at " << rateLimitPos
        << " and isIdempotentReadTool at " << cachePos;

    // Behavioural side: drive a cacheable tool to cap with the
    // test-only override.
    ClaudeIntegration::setRateLimitCapsForTest(/*cheap=*/3,
                                               /*expensive=*/2);
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("last_audit_summary");
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000 + i), 0);
    }
    EXPECT_GT(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1003), 0)
        << "INV-5: a cacheable tool at cap still refuses (the cache "
           "is downstream of the rate-limit gate)";
}

// INV-6 — empty buckets auto-pruned.
TEST_F(RateLimitTestFixture, Inv6EmptyBucketsAutoPruned) {
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("roadmap_query");
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 0);
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000), 0);
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 1);
    // Jump past the window — next check prunes the bucket; the
    // incoming call still pushes a fresh entry, so the count is 1
    // (not 0). To observe true count=0, we'd need to call check on
    // a DIFFERENT key after the original expired. Probe that path:
    EXPECT_EQ(ci.rateLimitCheck(QStringLiteral("tab_list"),
                                kCwdA, /*nowMs=*/61'001), 0);
    // tab_list bucket exists. roadmap_query is stale but only
    // pruned on access — count remains 2 until accessed.
    EXPECT_GE(ci.rateLimitBucketCountForTest(), 1);
    // Access roadmap_query again past the window — its bucket is
    // pruned (entire queue) then refilled with one fresh entry.
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/61'500), 0);
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(tool, kCwdA), 1)
        << "INV-6: stale entries pruned; only the fresh one remains";
}

// INV-7 — map cap LRU eviction.
TEST_F(RateLimitTestFixture, Inv7MapCapEviction) {
    ClaudeIntegration ci;
    // Fill the map to kRateLimitMapCap = 256 with distinct cwds.
    for (int i = 0; i < 256; ++i) {
        const QString cwd = QStringLiteral("/proj/%1").arg(i);
        ASSERT_EQ(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                    cwd, /*nowMs=*/1000 + i), 0);
    }
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 256);

    // 257th distinct key — must evict one.
    ASSERT_EQ(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                QStringLiteral("/proj/overflow"),
                                /*nowMs=*/2000), 0);
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 256)
        << "INV-7: map size must not exceed kRateLimitMapCap";
    // The oldest most-recent-timestamp was for /proj/0 at nowMs=1000.
    // It should have been evicted.
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(
        QStringLiteral("roadmap_query"),
        QStringLiteral("/proj/0")), 0)
        << "INV-7: /proj/0 (oldest most-recent) must be the eviction";
}

// INV-8 — per-(tool, cwd) isolation.
TEST_F(RateLimitTestFixture, Inv8PerToolCwdIsolation) {
    ClaudeIntegration::setRateLimitCapsForTest(/*cheap=*/60,
                                               /*expensive=*/2);
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("audit_run");
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000), 0);
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1001), 0);
    EXPECT_GT(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1002), 0)
        << "/proj/a bucket at cap";
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdB, /*nowMs=*/1002), 0)
        << "/proj/b bucket is independent";
    EXPECT_EQ(ci.rateLimitCheck(tool, kCwdB, /*nowMs=*/1003), 0);
    EXPECT_GT(ci.rateLimitCheck(tool, kCwdB, /*nowMs=*/1004), 0);
}

// INV-9 — caller-cwd-less buckets share.
TEST_F(RateLimitTestFixture, Inv9EmptyCwdSharesBucket) {
    ClaudeIntegration::setRateLimitCapsForTest(/*cheap=*/2,
                                               /*expensive=*/10);
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("roadmap_query");
    const QString emptyCwd;
    EXPECT_EQ(ci.rateLimitCheck(tool, emptyCwd, /*nowMs=*/1000), 0);
    EXPECT_EQ(ci.rateLimitCheck(tool, emptyCwd, /*nowMs=*/1001), 0);
    EXPECT_GT(ci.rateLimitCheck(tool, emptyCwd, /*nowMs=*/1002), 0)
        << "INV-9: two empty-cwd calls share the same bucket";
}

// INV-10 — refusal routed through recordDispatch (source-grep).
TEST_F(RateLimitTestFixture, Inv10RefusalRoutedThroughDispatch) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    // dispatchResult variable must be declared and set on the
    // rate-limit branch + passed to recordDispatch.
    EXPECT_NE(cc.find("QString dispatchResult"),
              std::string::npos);
    EXPECT_NE(cc.find("dispatchResult = QStringLiteral(\"rate_limited\")"),
              std::string::npos);
    EXPECT_NE(cc.find("dispatchResult);\n                    haveResult"),
              std::string::npos);
}

// INV-11 — refusal envelope shape (source-grep).
TEST_F(RateLimitTestFixture, Inv11EnvelopeShape) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    // The refusal envelope sets ok=false, code="rate_limited",
    // retry_after_ms, and an error string in that block.
    const auto rateLimitStart = cc.find(
        "// ANTS-1356 — per-tool sliding-window rate-limit.");
    ASSERT_NE(rateLimitStart, std::string::npos);
    const std::string region = cc.substr(rateLimitStart, 2000);
    EXPECT_NE(region.find("env[\"ok\"]              = false"),
              std::string::npos);
    EXPECT_NE(region.find("\"rate_limited\""), std::string::npos);
    EXPECT_NE(region.find("env[\"retry_after_ms\"]"), std::string::npos);
    EXPECT_NE(region.find("env[\"error\"]"), std::string::npos);
}

// INV-12 — caller_cwd_required runs before rate-limit (source-grep).
TEST_F(RateLimitTestFixture, Inv12OrderVsCallerCwdRequired) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto dispatchStart = cc.find("else if (method == \"tools/call\")");
    ASSERT_NE(dispatchStart, std::string::npos);
    // Window sized to comfortably span from the dispatch start through
    // the rate-limit block. Bumped 9000→12000 (ANTS-1772) — the
    // tool-existence + contract-gate prologue grew; the ordering
    // assertion below is what matters, not the exact window size.
    const std::string region = cc.substr(dispatchStart, 12000);
    const auto cwdRequired =
        region.find("CallerCwdContract::Required &&");
    const auto rateLimit = region.find("// ANTS-1356 — per-tool");
    ASSERT_NE(cwdRequired, std::string::npos);
    ASSERT_NE(rateLimit, std::string::npos);
    EXPECT_LT(cwdRequired, rateLimit)
        << "INV-12: caller_cwd_required check must precede rate-limit";
}

// INV-13 — tier classification source-grep. Confirms each
// expected tool appears in the classification function within
// the correct tier's surrounding context.
TEST_F(RateLimitTestFixture, Inv13TierClassification) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    // Isolate the rateLimitClassFor function body. The unqualified
    // identifier matches a call-site in processTools first; use the
    // qualified name + parameter list to anchor to the definition.
    const std::string classBody =
        ants_test::slurpFunctionBody(cc,
            "ClaudeIntegration::rateLimitClassFor(const QString &toolName)");
    ASSERT_FALSE(classBody.empty());

    // ControlPlane list — exactly five tools, each on a line that
    // also returns R::ControlPlane.
    const std::string cp[] = {
        "\"get_session_info\")",
        "\"token_usage\")",
        "\"tool_info\")",
        "\"mcp_trace\")",
        "\"caller_cwd_info\")"};
    for (const auto &needle : cp) {
        const auto pos = classBody.find(needle);
        ASSERT_NE(pos, std::string::npos)
            << "INV-13: ControlPlane allowlist must include " << needle;
        // Same line must return R::ControlPlane.
        const auto lineEnd = classBody.find('\n', pos);
        const std::string line = classBody.substr(pos, lineEnd - pos);
        EXPECT_NE(line.find("R::ControlPlane"), std::string::npos)
            << "INV-13: " << needle
            << " must map to R::ControlPlane; found line: " << line;
    }

    // Expensive list spot-check.
    // ANTS-1629 + ANTS-1643 — the three brief-assembly verbs
    // (cold_eyes_brief, indie_review_brief, test_audit_brief) moved
    // out of Expensive into BriefAssembly (30/min). Sibling
    // partition / corroborate / fold_in / dispatch verbs stay
    // Expensive.
    const std::string exp[] = {
        "\"audit_run\")",
        "\"workspace_search\")",
        "\"verify_changes\")",
        "\"indie_review_partition\")",
        "\"test_audit_partition\")",
        "\"debt_sweep_scan\")"};
    for (const auto &needle : exp) {
        const auto pos = classBody.find(needle);
        ASSERT_NE(pos, std::string::npos)
            << "INV-13: Expensive list missing " << needle;
        const auto lineEnd = classBody.find('\n', pos);
        const std::string line = classBody.substr(pos, lineEnd - pos);
        EXPECT_NE(line.find("R::Expensive"), std::string::npos)
            << "INV-13: " << needle
            << " must map to R::Expensive; found line: " << line;
    }

    // ANTS-1629 + ANTS-1643 — BriefAssembly tier (30/min). Three
    // tenants: cold_eyes_brief (1629), indie_review_brief (1643),
    // test_audit_brief (1643). Each must map to R::BriefAssembly.
    const std::string briefAssembly[] = {
        "\"cold_eyes_brief\")",
        "\"indie_review_brief\")",
        "\"test_audit_brief\")"};
    for (const auto &needle : briefAssembly) {
        const auto pos = classBody.find(needle);
        ASSERT_NE(pos, std::string::npos)
            << "INV-13 (ANTS-1629/1643): BriefAssembly list missing "
            << needle;
        const auto lineEnd = classBody.find('\n', pos);
        const std::string line = classBody.substr(pos, lineEnd - pos);
        EXPECT_NE(line.find("R::BriefAssembly"), std::string::npos)
            << "INV-13 (ANTS-1629/1643): " << needle
            << " must map to R::BriefAssembly; found line: " << line;
    }
}

// INV-14 — test-only cap override round-trip.
TEST_F(RateLimitTestFixture, Inv14TestOverrideRoundTrip) {
    ClaudeIntegration ci;
    // With default caps (60), 60 Cheap calls accept.
    for (int i = 0; i < 60; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                    kCwdA, /*nowMs=*/1000 + i), 0);
    }
    EXPECT_GT(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                kCwdA, /*nowMs=*/1060), 0);

    // Override Cheap cap to 3 — now the third call to a fresh key
    // fits, the fourth refuses.
    ClaudeIntegration::setRateLimitCapsForTest(/*cheap=*/3,
                                               /*expensive=*/2);
    ClaudeIntegration ci2;
    const QString tool = QStringLiteral("tab_list");
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(ci2.rateLimitCheck(tool, kCwdA, /*nowMs=*/2000 + i), 0);
    }
    EXPECT_GT(ci2.rateLimitCheck(tool, kCwdA, /*nowMs=*/2003), 0)
        << "INV-14: override applied — 4th call at cap=3 refuses";

    // Reset restores defaults — 60-call ceiling again on a fresh
    // instance.
    ClaudeIntegration::resetRateLimitCapsForTest();
    ClaudeIntegration ci3;
    for (int i = 0; i < 60; ++i) {
        ASSERT_EQ(ci3.rateLimitCheck(QStringLiteral("roadmap_query"),
                                     kCwdA, /*nowMs=*/3000 + i), 0)
            << "INV-14: reset restored default 60 cap; call " << i
            << " should accept";
    }
}

// INV-15 — probe accessors return correct values at each stage.
TEST_F(RateLimitTestFixture, Inv15ProbeAccessors) {
    ClaudeIntegration ci;
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 0);
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(
        QStringLiteral("roadmap_query"), kCwdA), 0);

    EXPECT_EQ(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                kCwdA, /*nowMs=*/1000), 0);
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 1);
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(
        QStringLiteral("roadmap_query"), kCwdA), 1);

    // After expiry + re-call, depth is 1 (fresh entry, stale pruned).
    EXPECT_EQ(ci.rateLimitCheck(QStringLiteral("roadmap_query"),
                                kCwdA, /*nowMs=*/61'500), 0);
    EXPECT_EQ(ci.rateLimitBucketDepthForTest(
        QStringLiteral("roadmap_query"), kCwdA), 1);
    EXPECT_EQ(ci.rateLimitBucketCountForTest(), 1);
}

// INV-16 — monotonic clock declared at TU scope.
TEST_F(RateLimitTestFixture, Inv16MonotonicClockDeclared) {
    const std::string cc = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    EXPECT_NE(cc.find("static QElapsedTimer s_rateLimitClock"),
              std::string::npos);
    EXPECT_NE(cc.find("s_rateLimitClock.start();"),
              std::string::npos);
    EXPECT_NE(cc.find("s_rateLimitClock.elapsed()"),
              std::string::npos);
}

// ANTS-1629 INV-17 — BriefAssembly tier accepts 30 calls and refuses
// the 31st. Drives `cold_eyes_brief` (the only BriefAssembly tenant
// at registration time) and proves a canonical /cold-eyes Phase-2
// fan-out of 12-16 briefs fits comfortably under the new cap.
TEST_F(RateLimitTestFixture, Inv17BriefAssemblyCapAt30) {
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("cold_eyes_brief");
    // 30 calls within the window must all accept.
    for (int i = 0; i < 30; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1000 + i), 0)
            << "INV-17: call " << i << " of 30 in BriefAssembly tier "
               "must accept";
    }
    // The 31st within the window must refuse with a positive
    // retry_after_ms.
    const qint64 retry = ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/1030);
    EXPECT_GT(retry, 0)
        << "INV-17: 31st call must refuse (BriefAssembly cap is 30/min)";
    // Sanity: the refusal points at a positive remaining-window value
    // bounded above by kRateLimitWindowMs.
    EXPECT_LE(retry, qint64{60'000});
}

// ANTS-1643 INV-17b — indie_review_brief + test_audit_brief siblings
// land in the same 30-cap BriefAssembly tier as cold_eyes_brief.
// Functional proof that the sibling-fan-out pattern works at 30
// before the orchestration report lands.
TEST_F(RateLimitTestFixture, Inv17bBriefAssemblyCoversSiblings) {
    ClaudeIntegration ci;
    const QString siblings[] = {
        QStringLiteral("indie_review_brief"),
        QStringLiteral("test_audit_brief"),
    };
    qint64 base = 10'000;
    for (const QString &tool : siblings) {
        // 30 calls within the window must all accept on a per-tool
        // (per-bucket) basis — sibling tools don't share a bucket.
        for (int i = 0; i < 30; ++i) {
            ASSERT_EQ(ci.rateLimitCheck(tool, kCwdA, base + i), 0)
                << "INV-17b: " << tool.toStdString()
                << " call " << i << " of 30 must accept";
        }
        // 31st must refuse.
        EXPECT_GT(ci.rateLimitCheck(tool, kCwdA, base + 30), 0)
            << "INV-17b: " << tool.toStdString()
            << " 31st call must refuse";
        base += 100'000;  // jump windows so per-tool buckets don't
                          // pollute the next sibling
    }
}

// ANTS-1629 INV-18 — 3-arg test-cap-override setter drives the
// BriefAssembly tier independently of Cheap / Expensive. Confirms
// the new override slot reads back through rateLimitTierFor.
TEST_F(RateLimitTestFixture, Inv18BriefAssemblyOverride) {
    ClaudeIntegration::setRateLimitCapsForTest(/*cheap=*/100,
                                               /*briefAssembly=*/4,
                                               /*expensive=*/100);
    ClaudeIntegration ci;
    const QString tool = QStringLiteral("cold_eyes_brief");
    // First four must accept under the synthetic cap of 4.
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/5000 + i), 0)
            << "INV-18: override cap=4 must allow first 4; call " << i;
    }
    // Fifth must refuse.
    EXPECT_GT(ci.rateLimitCheck(tool, kCwdA, /*nowMs=*/5004), 0)
        << "INV-18: 5th call must refuse under override cap=4";

    // Sibling tier (Cheap=100) still honours its own cap on the
    // same instance — confirms the override doesn't leak across tiers.
    const QString cheapTool = QStringLiteral("tab_list");
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(ci.rateLimitCheck(cheapTool, kCwdA,
                                    /*nowMs=*/6000 + i), 0)
            << "INV-18: Cheap tier honours its own (overridden) cap=100";
    }
}
