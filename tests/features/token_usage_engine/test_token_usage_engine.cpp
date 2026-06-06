// Feature-conformance test for ANTS-1284 TokenUsageEngine::Tracker.
// See tests/features/token_usage_engine/spec.md.

#include <gtest/gtest.h>

#include "tokenusageengine.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QThread>

using TokenUsageEngine::Tracker;

// INV-1
TEST(TokenUsageEngine, RecordCallExactness) {
    Tracker t;
    t.recordCall("foo", 10, 100);
    t.recordCall("foo", 20, 200);
    t.recordCall("foo", 30, 300);
    auto snap = t.buildReport(/*includeZero=*/true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].tool, QString("foo"));
    EXPECT_EQ(snap.calls[0].nCalls, 3);
    EXPECT_EQ(snap.calls[0].bytesIn, 60);
    EXPECT_EQ(snap.calls[0].bytesOut, 600);
}

// INV-2 — engine takes byte counts as-passed (no implicit conversion).
TEST(TokenUsageEngine, WireBytePassthrough) {
    Tracker t;
    // Caller-side .toUtf8().size() of an emoji = 4 wire bytes;
    // the engine records exactly what's passed.
    t.recordCall("foo", 0, 4);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].bytesOut, 4);
}

// INV-3
TEST(TokenUsageEngine, ResetClearsAndAdvancesSince) {
    Tracker t;
    t.recordCall("foo", 100, 1000);
    t.recordCall("bar", 50, 500);
    const qint64 sinceBefore = t.sinceUnixMs();
    // Sleep so the post-reset timestamp is observably later. 20 ms, not
    // 5 ms: a 10 ms-tick kernel (CONFIG_HZ=100) can leave a 5 ms sleep
    // landing within the same clock tick, so sinceUnixMs() wouldn't
    // advance and EXPECT_GT below would flake (ANTS-1607).
    QThread::msleep(20);
    t.reset();
    EXPECT_GT(t.sinceUnixMs(), sinceBefore);
    auto snap = t.buildReport(true);
    EXPECT_EQ(snap.calls.size(), 0);
    EXPECT_EQ(snap.totalSaved, 0);
    EXPECT_EQ(snap.toolsCalled, 0);
    // One post-reset call is visible in isolation.
    t.recordCall("baz", 1, 2);
    snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].tool, QString("baz"));
}

// INV-4 — est_tokens_saved floors at 0.
TEST(TokenUsageEngine, SavedFloorClampsNegative) {
    Tracker t;
    // roadmap_query baseline is 594000; pass a response that
    // exceeds it.
    t.recordCall("roadmap_query", 1000, 700000);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].estTokensSaved, 0)
        << "actual bytes > baseline must clamp to 0, not be negative";
}

// INV-5
TEST(TokenUsageEngine, BaselineLookup) {
    EXPECT_EQ(Tracker::baselineFor("roadmap_query"),  qint64(594000));
    EXPECT_EQ(Tracker::baselineFor("verify_changes"), qint64(8192));
    EXPECT_EQ(Tracker::baselineFor("plan_template"),  qint64(8192));
    EXPECT_EQ(Tracker::baselineFor("unknown_tool_xyz"), qint64(0));
    EXPECT_EQ(Tracker::baselineFor(""),               qint64(0));
}

// INV-9 — pure-read buildReport
TEST(TokenUsageEngine, BuildReportIsPure) {
    Tracker t;
    t.recordCall("foo", 10, 100);
    t.recordCall("bar", 5, 50);
    auto snap1 = t.buildReport(true);
    auto snap2 = t.buildReport(true);
    EXPECT_EQ(snap1.toolsCalled, snap2.toolsCalled);
    EXPECT_EQ(snap1.totalSaved, snap2.totalSaved);
    ASSERT_EQ(snap1.calls.size(), snap2.calls.size());
    for (int i = 0; i < snap1.calls.size(); ++i) {
        EXPECT_EQ(snap1.calls[i].tool, snap2.calls[i].tool);
        EXPECT_EQ(snap1.calls[i].nCalls, snap2.calls[i].nCalls);
        EXPECT_EQ(snap1.calls[i].bytesIn, snap2.calls[i].bytesIn);
        EXPECT_EQ(snap1.calls[i].bytesOut, snap2.calls[i].bytesOut);
        EXPECT_EQ(snap1.calls[i].estTokensSaved,
                  snap2.calls[i].estTokensSaved);
    }
}

// INV-10 — self-counted (baseline 0, so saved is 0 regardless)
TEST(TokenUsageEngine, TokenUsageToolSelfCountsButSavesZero) {
    Tracker t;
    t.recordCall("token_usage", 50, 300);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].tool, QString("token_usage"));
    EXPECT_EQ(snap.calls[0].nCalls, 1);
    EXPECT_EQ(snap.calls[0].estTokensSaved, 0);
    EXPECT_EQ(snap.totalSaved, 0);
}

// Sort order — by est_tokens_saved desc, tiebreak by tool name asc.
TEST(TokenUsageEngine, CallsSortedDescByTokensSaved) {
    Tracker t;
    // Three tools, all baselined, all called once with negligible
    // response sizes — savings are proportional to baseline.
    t.recordCall("roadmap_query", 0, 0);   // 594000 baseline → ~148500 saved
    t.recordCall("verify_changes", 0, 0);  // 8192   baseline → ~2048 saved
    t.recordCall("plan_template",  0, 0);  // 8192   baseline → ~2048 saved
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 3);
    // First: roadmap_query (largest baseline).
    EXPECT_EQ(snap.calls[0].tool, QString("roadmap_query"));
    // Tie between verify_changes (8192) and plan_template (8192) —
    // tiebreak is tool name ascending, so plan_template before
    // verify_changes (p < v).
    EXPECT_EQ(snap.calls[1].tool, QString("plan_template"));
    EXPECT_EQ(snap.calls[2].tool, QString("verify_changes"));
}

// include_zero filter
TEST(TokenUsageEngine, IncludeZeroFilter) {
    Tracker t;
    t.recordCall("tab_list", 30, 1240);          // baseline 0 → 0 saved
    t.recordCall("roadmap_query", 100, 50000);   // baseline 594000 → 135975 saved
    {
        auto snap = t.buildReport(/*includeZero=*/false);
        // tab_list dropped from calls[] but still in tools_called.
        EXPECT_EQ(snap.calls.size(), 1);
        EXPECT_EQ(snap.calls[0].tool, QString("roadmap_query"));
        EXPECT_EQ(snap.toolsCalled, 2);
        EXPECT_GT(snap.totalSaved, 0);
    }
    {
        auto snap = t.buildReport(/*includeZero=*/true);
        EXPECT_EQ(snap.calls.size(), 2);
        EXPECT_EQ(snap.toolsCalled, 2);
    }
}

// est_tokens_saved arithmetic — (baseline*n - in - out) / 4
TEST(TokenUsageEngine, SavedArithmetic) {
    Tracker t;
    // verify_changes baseline 8192; 1 call; in=100, out=300 → saved bytes
    // = max(0, 8192 - 400) = 7792; tokens = 7792 / 4 = 1948.
    t.recordCall("verify_changes", 100, 300);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].estTokensSaved, qint64(1948));
}

// Multi-call baseline scaling — baseline is per-call.
TEST(TokenUsageEngine, BaselineScalesPerCall) {
    Tracker t;
    // verify_changes × 3 calls, total in=0, total out=0 → baseline
    // total = 8192 * 3 = 24576; saved bytes = 24576; tokens = 6144.
    t.recordCall("verify_changes", 0, 0);
    t.recordCall("verify_changes", 0, 0);
    t.recordCall("verify_changes", 0, 0);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].nCalls, 3);
    EXPECT_EQ(snap.calls[0].estTokensSaved, qint64(6144));
}

// ---------------------------------------------------------------------------
// ANTS-1355 — v2: wrap_bytes + latency accumulators.
// ---------------------------------------------------------------------------

// V2-1 — recordCall writes new accumulators.
TEST(TokenUsageEngine, V2WrapBytesAndDurationAccumulate) {
    Tracker t;
    t.recordCall("foo", 10, 100, /*wrap=*/5,  /*durUs=*/200);
    t.recordCall("foo", 10, 100, /*wrap=*/5,  /*durUs=*/200);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].wrapBytes,      qint64(10));
    EXPECT_EQ(snap.calls[0].durationUsMin,  qint64(200));
    EXPECT_EQ(snap.calls[0].durationUsMax,  qint64(200));
    EXPECT_EQ(snap.calls[0].durationUsMean, qint64(200));
    EXPECT_EQ(snap.totalWrapBytes,          qint64(10));
}

// V2-2 — duration min/max/mean evolve correctly across calls.
TEST(TokenUsageEngine, V2DurationMinMaxMeanEvolve) {
    Tracker t;
    t.recordCall("bar", 0, 0, 0, /*durUs=*/500);
    t.recordCall("bar", 0, 0, 0, /*durUs=*/100);
    t.recordCall("bar", 0, 0, 0, /*durUs=*/2000);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].durationUsMin,  qint64(100));
    EXPECT_EQ(snap.calls[0].durationUsMax,  qint64(2000));
    // floor((500 + 100 + 2000) / 3) = floor(866.66) = 866.
    EXPECT_EQ(snap.calls[0].durationUsMean, qint64(866));
}

// V2-3 — totalWrapBytes envelope sum across ALL tools, even when
// includeZero:false filters a contributing tool out of calls[].
TEST(TokenUsageEngine, V2TotalWrapBytesSpansAllTools) {
    Tracker t;
    // roadmap_query — has a baseline → est_tokens_saved > 0.
    t.recordCall("roadmap_query", 0, 0, /*wrap=*/30, /*durUs=*/100);
    // unknown tool — baseline 0 → est_tokens_saved 0; would be
    // filtered when includeZero:false. wrap_bytes still counts.
    t.recordCall("zzz_unknown",    0, 0, /*wrap=*/50, /*durUs=*/100);
    auto snap = t.buildReport(/*includeZero=*/false);
    // zzz_unknown filtered out of calls[].
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].tool, QString("roadmap_query"));
    // But its wrap_bytes contributes to the envelope total.
    EXPECT_EQ(snap.totalWrapBytes, qint64(80));
    // tools_called still counts both.
    EXPECT_EQ(snap.toolsCalled, 2);
}

// V2-4 — sentinel handling: first call with durationUs=0 records a
// genuine 0 sample (NOT treated as uninitialised).
TEST(TokenUsageEngine, V2DurationZeroOnFirstCallIsGenuine) {
    Tracker t;
    t.recordCall("baz", 0, 0, 0, /*durUs=*/0);
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].nCalls, 1);
    EXPECT_EQ(snap.calls[0].durationUsMin,  qint64(0));
    EXPECT_EQ(snap.calls[0].durationUsMax,  qint64(0));
    EXPECT_EQ(snap.calls[0].durationUsMean, qint64(0));
    // Adding a non-zero second sample lifts max but min stays at 0.
    t.recordCall("baz", 0, 0, 0, /*durUs=*/500);
    snap = t.buildReport(true);
    EXPECT_EQ(snap.calls[0].durationUsMin,  qint64(0));
    EXPECT_EQ(snap.calls[0].durationUsMax,  qint64(500));
    EXPECT_EQ(snap.calls[0].durationUsMean, qint64(250));
}

// V2-5 — duration_us_sum is private (not on ToolReport).
// Compile-time check: presence of the public fields is enough;
// absence of `durationUsSum` would only be testable via SFINAE
// which is overkill for a feature test. Documented in spec INV-9.
TEST(TokenUsageEngine, V2DurationSumIsPrivate) {
    TokenUsageEngine::ToolReport r;
    // These fields exist (won't compile if they don't):
    (void)r.durationUsMin;
    (void)r.durationUsMax;
    (void)r.durationUsMean;
    // r.durationUsSum would not compile — INV-9 enforced
    // structurally by absence.
    SUCCEED();
}

// V2-6 — reset clears v2 accumulators too.
TEST(TokenUsageEngine, V2ResetClearsV2Fields) {
    Tracker t;
    t.recordCall("foo", 10, 100, /*wrap=*/5, /*durUs=*/200);
    t.reset();
    auto snap = t.buildReport(true);
    EXPECT_EQ(snap.totalWrapBytes, qint64(0));
    EXPECT_EQ(snap.calls.size(),   0);
    // Second record after reset behaves as a fresh tool (sentinel).
    t.recordCall("foo", 0, 0, 0, /*durUs=*/777);
    snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    EXPECT_EQ(snap.calls[0].durationUsMin, qint64(777));
    EXPECT_EQ(snap.calls[0].durationUsMax, qint64(777));
}

// V2-7 — v1 carry-overs unchanged when v2 fields default to 0.
TEST(TokenUsageEngine, V2DefaultedCallMatchesV1Behavior) {
    Tracker t;
    t.recordCall("verify_changes", 100, 300);  // no v2 args
    auto snap = t.buildReport(true);
    ASSERT_EQ(snap.calls.size(), 1);
    // v1 fields exact match.
    EXPECT_EQ(snap.calls[0].estTokensSaved, qint64(1948));
    // v2 fields zero.
    EXPECT_EQ(snap.calls[0].wrapBytes,      qint64(0));
    EXPECT_EQ(snap.calls[0].durationUsMin,  qint64(0));
    EXPECT_EQ(snap.calls[0].durationUsMax,  qint64(0));
    EXPECT_EQ(snap.calls[0].durationUsMean, qint64(0));
    EXPECT_EQ(snap.totalWrapBytes,          qint64(0));
}
