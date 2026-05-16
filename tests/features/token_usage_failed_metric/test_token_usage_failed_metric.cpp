// Feature-conformance test for ANTS-1432 failed-call metric on
// TokenUsageEngine::Tracker.
// See tests/features/token_usage_failed_metric/spec.md.

#include <gtest/gtest.h>

#include "tokenusageengine.h"

#include <QString>

using TokenUsageEngine::Tracker;

// INV-1 — failed branch only touches failed-* fields.
TEST(TokenUsageFailedMetric, FailedBranchIsolates) {
    Tracker t;
    t.recordCall("roadmap_query",
                 /*bytesIn=*/47, /*bytesOut=*/318,
                 /*wrapBytes=*/0, /*durationUs=*/0,
                 /*success=*/false);
    auto snap = t.buildReport(/*includeZero=*/true);
    ASSERT_EQ(snap.calls.size(), 1);
    const auto &r = snap.calls[0];
    EXPECT_EQ(r.tool,           QString("roadmap_query"));
    // Success-side accumulators untouched.
    EXPECT_EQ(r.nCalls,         0);
    EXPECT_EQ(r.bytesIn,        qint64(0));
    EXPECT_EQ(r.bytesOut,       qint64(0));
    EXPECT_EQ(r.wrapBytes,      qint64(0));
    EXPECT_EQ(r.durationUsMin,  qint64(0));
    EXPECT_EQ(r.durationUsMax,  qint64(0));
    EXPECT_EQ(r.durationUsMean, qint64(0));
    EXPECT_EQ(r.estTokensSaved, qint64(0));
    // Failed-side accumulators incremented.
    EXPECT_EQ(r.failedCalls,    qint64(1));
    EXPECT_EQ(r.failedBytesIn,  qint64(47));
    EXPECT_EQ(r.failedBytesOut, qint64(318));
}

// INV-2 — success branch only touches success-* fields.
TEST(TokenUsageFailedMetric, SuccessBranchIsolates) {
    Tracker t;
    t.recordCall("verify_changes",
                 /*bytesIn=*/100, /*bytesOut=*/300,
                 /*wrapBytes=*/12, /*durationUs=*/250,
                 /*success=*/true);
    auto snap = t.buildReport(/*includeZero=*/true);
    ASSERT_EQ(snap.calls.size(), 1);
    const auto &r = snap.calls[0];
    // Success-side: existing behaviour.
    EXPECT_EQ(r.nCalls,         1);
    EXPECT_EQ(r.bytesIn,        qint64(100));
    EXPECT_EQ(r.bytesOut,       qint64(300));
    EXPECT_EQ(r.wrapBytes,      qint64(12));
    EXPECT_EQ(r.durationUsMin,  qint64(250));
    EXPECT_EQ(r.durationUsMax,  qint64(250));
    EXPECT_EQ(r.durationUsMean, qint64(250));
    // verify_changes baseline 8192 → saved = (8192 - 400) / 4 = 1948.
    EXPECT_EQ(r.estTokensSaved, qint64(1948));
    // Failed-side: zero.
    EXPECT_EQ(r.failedCalls,    qint64(0));
    EXPECT_EQ(r.failedBytesIn,  qint64(0));
    EXPECT_EQ(r.failedBytesOut, qint64(0));
}

// INV-3 — totalFailedBytes spans ALL tools, including ones filtered
// out of calls[] by include_zero:false.
TEST(TokenUsageFailedMetric, TotalFailedBytesSpansAllTools) {
    Tracker t;
    // Successful baseline tool — survives include_zero:false filter.
    t.recordCall("roadmap_query",
                 /*bytesIn=*/100, /*bytesOut=*/500,
                 /*wrapBytes=*/0, /*durationUs=*/0,
                 /*success=*/true);
    // Failed call on a different tool — also survives include_zero
    // filter (INV-4) because failedCalls > 0.
    t.recordCall("workspace_search",
                 /*bytesIn=*/30, /*bytesOut=*/120,
                 /*wrapBytes=*/0, /*durationUs=*/0,
                 /*success=*/false);
    // Failed call on a zero-saved tool — dropped from calls[] by
    // include_zero filter (both zero) only if it has zero failed
    // calls too; here it has 1 failed call, so it should appear.
    t.recordCall("zzz_unknown_tool",
                 /*bytesIn=*/5, /*bytesOut=*/25,
                 /*wrapBytes=*/0, /*durationUs=*/0,
                 /*success=*/false);

    auto snap = t.buildReport(/*includeZero=*/false);
    // All three tools survive include_zero:false: one via
    // estTokensSaved > 0, two via failedCalls > 0.
    EXPECT_EQ(snap.toolsCalled, 3);
    EXPECT_EQ(snap.calls.size(), 3);
    // Envelope summary aggregates ALL three tools' failed bytes.
    // workspace_search: 30 + 120 = 150; zzz_unknown_tool: 5 + 25 = 30.
    // roadmap_query had zero failed bytes.
    EXPECT_EQ(snap.totalFailedBytes, qint64(150 + 30));
}

// INV-4 — include_zero:false retains tools with failed-only history.
// A tool with both success and failure counters at zero would not
// appear, but the engine never creates such an entry (recordCall is
// the only mutator and always increments at least one side).
TEST(TokenUsageFailedMetric, IncludeZeroRetainsFailureOnlyTools) {
    Tracker t;
    // Failed-only tool with zero baseline (so estTokensSaved == 0).
    t.recordCall("zzz_unknown_tool",
                 /*bytesIn=*/10, /*bytesOut=*/40,
                 /*wrapBytes=*/0, /*durationUs=*/0,
                 /*success=*/false);
    {
        auto snap = t.buildReport(/*includeZero=*/false);
        // Pre-1432 behaviour would have dropped this tool entirely
        // because estTokensSaved == 0. Post-1432 it survives because
        // failedCalls > 0.
        ASSERT_EQ(snap.calls.size(), 1);
        EXPECT_EQ(snap.calls[0].tool, QString("zzz_unknown_tool"));
        EXPECT_EQ(snap.calls[0].failedCalls, qint64(1));
        EXPECT_EQ(snap.calls[0].estTokensSaved, qint64(0));
    }
    // include_zero:true equivalent must also surface it (and would
    // surface it pre-1432 too).
    {
        auto snap = t.buildReport(/*includeZero=*/true);
        ASSERT_EQ(snap.calls.size(), 1);
        EXPECT_EQ(snap.calls[0].failedCalls, qint64(1));
    }
}

// Mixed-history sanity — a tool that has both successful and failed
// dispatches surfaces both counters side-by-side.
TEST(TokenUsageFailedMetric, MixedSuccessAndFailureCoexist) {
    Tracker t;
    t.recordCall("roadmap_query", 100, 500, 0, 0, /*success=*/true);
    t.recordCall("roadmap_query", 50, 250, 0, 0, /*success=*/false);
    t.recordCall("roadmap_query", 60, 280, 0, 0, /*success=*/false);
    auto snap = t.buildReport(/*includeZero=*/true);
    ASSERT_EQ(snap.calls.size(), 1);
    const auto &r = snap.calls[0];
    // Success side: 1 call, 100/500 bytes.
    EXPECT_EQ(r.nCalls,         1);
    EXPECT_EQ(r.bytesIn,        qint64(100));
    EXPECT_EQ(r.bytesOut,       qint64(500));
    // Failed side: 2 calls, 110/530 bytes.
    EXPECT_EQ(r.failedCalls,    qint64(2));
    EXPECT_EQ(r.failedBytesIn,  qint64(110));
    EXPECT_EQ(r.failedBytesOut, qint64(530));
    // Envelope sum across the single tool.
    EXPECT_EQ(snap.totalFailedBytes, qint64(110 + 530));
}

// Default `success` arg keeps existing call sites compatible.
TEST(TokenUsageFailedMetric, DefaultSuccessArgPreservesV2Behavior) {
    Tracker t;
    // No `success` arg → defaults to true. This is the v2 (ANTS-1355)
    // call shape; ANTS-1432 must not break it.
    t.recordCall("verify_changes", 100, 300);
    auto snap = t.buildReport(/*includeZero=*/true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].nCalls,       1);
    EXPECT_EQ(snap.calls[0].failedCalls,  qint64(0));
    EXPECT_EQ(snap.calls[0].estTokensSaved, qint64(1948));
}
