// ANTS-1436 — pagination + auto-truncate feature test.
// Pure-function tests on PaginationEngine::pageBullets +
// source-scrape on cmdRoadmapQuery wiring.

#include "../../_support/expect.h"
#include "paginationengine.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Build N synthetic bullet objects of ~120 B each so they fit comfortably
// under the soft cap individually but a long array breaches it.
QJsonArray makeBullets(int n, int payloadBytes = 100) {
    QJsonArray arr;
    const QString pad(payloadBytes, QLatin1Char('x'));
    for (int i = 0; i < n; ++i) {
        QJsonObject o;
        o["id"]       = QStringLiteral("ANTS-%1").arg(i, 4, 10,
                                                       QLatin1Char('0'));
        o["status"]   = QStringLiteral("📋");
        o["headline"] = pad;
        o["kind"]     = QStringLiteral("test");
        o["lanes"]    = QJsonArray();
        arr.append(o);
    }
    return arr;
}

}  // namespace

// INV-1 / engine — small array, no caller args, no truncation.
TEST(roadmap_query_pagination, Inv1SmallArrayNoTruncation) {
    const auto arr = makeBullets(5);
    const auto r = PaginationEngine::pageBullets(arr, 0, -1);
    EXPECT_EQ(r.slice.size(), 5);
    EXPECT_EQ(r.offset, 0);
    EXPECT_EQ(r.total, 5);
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.nextOffset, -1);
    EXPECT_FALSE(PaginationEngine::shouldEmitPaginationFields(
        /*offset*/false, /*limit*/false, r.truncated));
}

// INV-2 — explicit limit echoes back.
TEST(roadmap_query_pagination, Inv2ExplicitLimitEchoes) {
    const auto arr = makeBullets(100);
    const auto r = PaginationEngine::pageBullets(arr, 10, 20);
    EXPECT_EQ(r.slice.size(), 20);
    EXPECT_EQ(r.offset, 10);
    EXPECT_EQ(r.limit, 20);
    EXPECT_EQ(r.total, 100);
    EXPECT_TRUE(r.truncated);  // 10+20 = 30 < 100
    EXPECT_EQ(r.nextOffset, 30);
}

// INV-3 — large filtered + no caller limit triggers auto-truncate.
TEST(roadmap_query_pagination, Inv3AutoTruncateOnLargeFiltered) {
    // 500 bullets at ~120 B each = ~60 KB, well over 20 KB cap.
    const auto arr = makeBullets(500);
    const auto r = PaginationEngine::pageBullets(arr, 0, -1);
    EXPECT_LT(r.slice.size(), arr.size());
    EXPECT_TRUE(r.truncated);
    EXPECT_GT(r.nextOffset, 0);
    EXPECT_LT(r.nextOffset, 500);
    // The serialized slice should fit under the cap.
    const auto bytes = QJsonDocument(r.slice).toJson(QJsonDocument::Compact).size();
    EXPECT_LE(bytes, PaginationEngine::kSoftCapBytes
                  - PaginationEngine::kEnvelopeOverheadBytes);
}

// INV-4 — next_offset = offset + slice.length.
TEST(roadmap_query_pagination, Inv4NextOffsetAdvances) {
    const auto arr = makeBullets(50);
    const auto r = PaginationEngine::pageBullets(arr, 5, 10);
    ASSERT_TRUE(r.truncated);
    EXPECT_EQ(r.nextOffset, r.offset + r.slice.size());
}

// INV-5 — offset past end returns empty.
TEST(roadmap_query_pagination, Inv5OffsetPastEndEmpty) {
    const auto arr = makeBullets(10);
    const auto r = PaginationEngine::pageBullets(arr, 100, 5);
    EXPECT_EQ(r.slice.size(), 0);
    EXPECT_EQ(r.offset, 10);  // clamped to total
    EXPECT_EQ(r.total, 10);
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.nextOffset, -1);
}

// INV-9 — explicit limit honoured up to clamp (500).
TEST(roadmap_query_pagination, Inv9ExplicitLimitHonoured) {
    const auto arr = makeBullets(450);
    // Caller asks for 400 even though auto-pick would pick less;
    // explicit limit wins.
    const auto r = PaginationEngine::pageBullets(arr, 0, 400);
    EXPECT_EQ(r.slice.size(), 400);
    EXPECT_EQ(r.limit, 400);
    EXPECT_TRUE(r.truncated);  // 400 < 450 total
}

// INV-9 clamp — limit above 500 is clamped to 500.
TEST(roadmap_query_pagination, Inv9LimitClampedTo500) {
    const auto arr = makeBullets(600);
    const auto r = PaginationEngine::pageBullets(arr, 0, 9999);
    EXPECT_EQ(r.limit, PaginationEngine::kMaxLimit);
    EXPECT_EQ(r.slice.size(), PaginationEngine::kMaxLimit);
}

// Edge — single bullet that exceeds budget on its own.
TEST(roadmap_query_pagination, EdgePathologicalBullet) {
    // One bullet with a payload larger than the cap.
    const auto arr = makeBullets(1, /*payloadBytes*/30000);
    const auto r = PaginationEngine::pageBullets(arr, 0, -1);
    // Cut point is 0 — even one bullet doesn't fit.
    EXPECT_EQ(r.slice.size(), 0);
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(r.nextOffset, 0);  // caller would need to skip past it manually
}

// Empty input returns empty slice; no truncation.
TEST(roadmap_query_pagination, EmptyArrayNoTruncation) {
    const QJsonArray arr;
    const auto r = PaginationEngine::pageBullets(arr, 0, -1);
    EXPECT_EQ(r.slice.size(), 0);
    EXPECT_EQ(r.total, 0);
    EXPECT_FALSE(r.truncated);
}

// Source-scrape — INV-8 bad_args + dispatch + helper call sites.
TEST(roadmap_query_pagination, Inv8BadArgsAnchors) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(contains(cpp, "ANTS-1436-INV-8"),
           "INV-8: bad_args anchor present in cmdRoadmapQuery");
    expect(contains(cpp, "offset must be a non-negative integer"),
           "INV-8: bad_args message for offset");
    expect(contains(cpp, "limit must be a positive integer"),
           "INV-8: bad_args message for limit");
    EXPECT_EQ(0, expect_failures());
}

// INV-6 (ANTS-1729) — section_index now ACCEPTS offset/limit and
// paginates the sections[] array (superseding the ANTS-1436-INV-6
// rejection). The refusal message must be GONE and the section_index
// branch must route through the pagination helper.
TEST(roadmap_query_pagination, Inv6SectionIndexPaginates) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    expect(!contains(cpp, "section_index mode does not accept offset/limit"),
           "INV-6: the old section_index+offset/limit refusal is removed "
           "(ANTS-1729 enables pagination)");
    expect(contains(cpp, "ANTS-1729"),
           "INV-6: ANTS-1729 pagination anchor present in the "
           "section_index branch");
    EXPECT_EQ(0, expect_failures());
}

// INV-11 — PaginationEngine::pageBullets called from exactly five
// sites in remotecontrol.cpp: three in cmdRoadmapQuery (section +
// full-file bullet emission + section_index sections[], ANTS-1729)
// and two in cmdChangelogQuery (entries + version_index, ANTS-3533).
TEST(roadmap_query_pagination, Inv11HelperCallSiteCount) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    size_t count = 0;
    size_t pos = 0;
    const std::string needle = "PaginationEngine::pageBullets(";
    while ((pos = cpp.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    expect(count == 5,
           "INV-11: PaginationEngine::pageBullets must be called "
           "exactly 5 times in remotecontrol.cpp (3 in cmdRoadmapQuery + "
           "2 in cmdChangelogQuery). If you added another emission "
           "branch, wire it through the helper too.");
    EXPECT_EQ(0, expect_failures());
}

// Dispatch forward — offset/limit reach cmdRoadmapQuery. ANTS-3422
// replaced the hand-maintained per-arg forward with a verbatim
// `rcDelegate` forward that passes the whole args object through, so
// offset/limit (and every other arg) arrive un-gated and the handler
// still owns the bad_args check on non-numeric input.
TEST(roadmap_query_pagination, DispatchForwardsVerbatim) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "rcDelegate(&RemoteControl::cmdRoadmapQuery)"),
           "dispatch: roadmap_query registered via the verbatim rcDelegate "
           "forward, so offset/limit reach the handler un-gated");
    expect(contains(mw, "ANTS-3422"),
           "dispatch: ANTS-3422 verbatim-forward anchor present");
    EXPECT_EQ(0, expect_failures());
}

// Schema — offset/limit props advertised on roadmap_query tool.
TEST(roadmap_query_pagination, SchemaPaginationPropsAdvertised) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "ANTS-1436 — offset/limit"),
           "schema: ANTS-1436 anchor in claudeintegration");
    expect(contains(cpp, "props[\"offset\"] = offsetProp"),
           "schema: offset property registered");
    expect(contains(cpp, "props[\"limit\"] = limitProp"),
           "schema: limit property registered");
    EXPECT_EQ(0, expect_failures());
}
