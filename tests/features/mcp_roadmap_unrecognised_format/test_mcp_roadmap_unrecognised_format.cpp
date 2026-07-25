// ANTS-1429 — feature-conformance test for the unrecognised_format
// gate on cmdRoadmapQuery + cmdRoadmapLog. Source-scrape pattern;
// behavioural surface covered by remote_control_roadmap_query.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Bounded substring between two signatures. Returns the substring
// from the first signature's position up to the second's. Asserts
// the bound is non-zero and ≤ kMaxBound bytes so a future reorder
// that widens the bound trips the test loudly rather than passing
// silently against the wrong function body.
// ANTS-1428 bumped the kMaxBound ceiling from 16 KB to 24 KB
// because cmdRoadmapQuery legitimately grew: per-bullet
// `format`/`synthetic`/`anchor` envelope fields + the
// envelope-level format echo are ~700 B of new behaviour
// distributed across the cache-fill, lazy-fill, and emission
// paths. ANTS-1437 + ANTS-1438 bumped it again to 28 KB:
// section_index branch (~4 KB — lazy fill + tally + envelope)
// plus bold_id emissions across three sites (~300 B). The bound
// is still a meaningful runaway-guard ceiling — at 28 KB any
// "added a new mode" change should make the author think twice
// about whether the work belongs in a helper instead.
std::string boundedBetween(const std::string &cpp,
                           const std::string &startSig,
                           const std::string &endSig,
                           size_t kMaxBound = 96 * 1024) {
    // ANTS-1436 bumped 28→32 KB: pagination args parse (~1.5 KB)
    // + PaginationEngine::pageBullets call + envelope augment at
    // each of the 2 emission sites (~600 B × 2). The pagination
    // LOGIC lives in src/paginationengine.cpp; only the call sites
    // are in cmdRoadmapQuery.
    // ANTS-1462 bumped 32→40 KB: header-inventory fallback adds
    // a lazy buildIndex call + ~1.5 KB envelope construction in
    // each of the 2 emission sites in cmdRoadmapQuery. The
    // inventory rendering itself lives in
    // buildHeaderInventoryEnvelope outside the function body.
    // ANTS-1622 bumped 40→44 KB: section_index emission grew by
    // ~1.5 KB to surface the three `*_id_only` parallel counts
    // per section + the top-level legacy_format_sections[] hint
    // (the tally pass itself only adds ~200 B; the bulk is in
    // the per-section emission and the inline rationale comment).
    // ANTS-1856 bumped 44→52 KB: the single-item `id` selector adds
    // the id parse + two combo-rejection guards + the bypass branch
    // (~3.7 KB) ahead of the status filter. cmdRoadmapQuery is now
    // ~48.7 KB — still a meaningful runaway ceiling.
    // ANTS-1726 bumped 52→64 KB: the plural `ids` selector adds the
    // ids parse + three combo guards + the document-order match
    // branch with matched/missing accounting (~5 KB total).
    // cmdRoadmapQuery is now ~57 KB.
    // ANTS-2052 bumped 64→72 KB: the section_index legacy-roadmap
    // fallback adds the raw-total sum + id-only survivor pre-count +
    // the raw/id-only drop-predicate switch + the legacy_format/
    // raw_*_count envelope emission (~2 KB). Body is now ~66 KB —
    // the ceiling stays a meaningful runaway guard.
    // ANTS-3400/3402 bumped 72→80 KB: the accepted-status-filter block +
    // sectionFilter collapse + granular lifecycle predicate branches
    // (ANTS-3400) and the max_body_bytes parse + rcCapBodyFields emission
    // caps (ANTS-3402) add ~4 KB.
    // ANTS-3425 bumped 80→88 KB: the three m_roadmapCacheBullets builders
    // now pass kRoadmapQueryBodyStoreCap + carry the rationale comment so
    // an id/ids max_body_bytes fetch is no longer inert (~0.5 KB). The
    // measured body sat at ~79.5 KB before this change (the earlier
    // "~70 KB" note was stale), so it had crept to the ceiling; the
    // guard stays meaningful and still flags a genuine new-mode addition.
    // ANTS-3617 bumped 88→96 KB: collapsing the four-way `mode !=` chain
    // into a single kModes list (so bad_mode can echo `accepted`, matching
    // changelog_query) plus its rationale comment added ~0.7 KB. The
    // measured body was 89,497 B before the change and 90,168 B after —
    // i.e. it had crept to within 600 B of the old 90,112 B ceiling, and
    // this bump restores the headroom rather than papering over a
    // runaway.
    const auto startPos = cpp.find(startSig);
    if (startPos == std::string::npos) return {};
    const auto endPos = cpp.find(endSig, startPos + startSig.size());
    if (endPos == std::string::npos) return {};
    const auto len = endPos - startPos;
    if (len == 0 || len > kMaxBound) return {};
    return cpp.substr(startPos, len);
}

}  // namespace

// INV-1 — cmdRoadmapQuery gate.
TEST(mcp_roadmap_unrecognised_format, Inv1QueryGate) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-1: remotecontrol.cpp not readable";

    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapQuery(",
        "QJsonDocument RemoteControl::cmdRoadmapLog(");
    ASSERT_FALSE(body.empty())
        << "INV-1: failed to bound cmdRoadmapQuery body (sig moved "
           "or body exceeded boundedBetween's kMaxBound ceiling)";

    expect(contains(body, "ANTS-1429"),
           "INV-1: ANTS-1429 anchor present in cmdRoadmapQuery");
    expect(contains(body, "\"unrecognised_format\""),
           "INV-1: unrecognised_format literal in cmdRoadmapQuery");
    expect(contains(body, "kRoadmapMinParseableSize"),
           "INV-1: kRoadmapMinParseableSize threshold referenced");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — threshold constant in the header.
TEST(mcp_roadmap_unrecognised_format, Inv2Threshold) {
    expect_reset();
    const std::string hdr = ants_test::slurpFile(SRC_REMOTECONTROL_H_PATH);
    ASSERT_FALSE(hdr.empty()) << "INV-2: remotecontrol.h not readable";

    expect(contains(hdr, "kRoadmapMinParseableSize"),
           "INV-2: kRoadmapMinParseableSize symbol present in "
           "remotecontrol.h");
    expect(contains(hdr, "= 1024"),
           "INV-2: 1 KB threshold value (= 1024) present in "
           "remotecontrol.h");
    expect(contains(hdr, "kRoadmapCacheTtlMs"),
           "INV-2: kRoadmapCacheTtlMs still co-located so the "
           "constant cluster stays intact");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — cmdRoadmapLog gate + envelope shape parity.
TEST(mcp_roadmap_unrecognised_format, Inv3LogGateAndShape) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-3: remotecontrol.cpp not readable";

    // The `unrecognised_format` WRITE gate lives in cmdRoadmapLogAppend
    // (cmdRoadmapLog itself is now a thin dispatcher). Bound that function
    // directly — bounding from the dispatcher instead swept up the unrelated
    // cmdChangelog* functions that sit between it and cmdRoadmapLogFlip in the
    // TU, so any growth there (e.g. ANTS-3584 add_subsection) tipped the
    // accidental span over the boundedBetween ceiling (ANTS source-scrape
    // window brittleness).
    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapLogAppend(",
        "QJsonDocument RemoteControl::cmdRoadmapLogFlip(");
    ASSERT_FALSE(body.empty())
        << "INV-3: failed to bound cmdRoadmapLogAppend body (sig moved "
           "or bound exceeded)";

    expect(contains(body, "ANTS-1429"),
           "INV-3: ANTS-1429 anchor present in cmdRoadmapLog");
    expect(contains(body, "\"unrecognised_format\""),
           "INV-3: unrecognised_format literal in cmdRoadmapLog");
    // Envelope shape parity — read and write paths both carry path
    // + bytes fields. Inline construction (not via rlErr) is the
    // signal that the parity is intentional.
    expect(contains(body, "env[\"path\"]"),
           "INV-3: env[\"path\"] assigned in cmdRoadmapLog gate");
    expect(contains(body, "env[\"bytes\"]"),
           "INV-3: env[\"bytes\"] assigned in cmdRoadmapLog gate");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1462 — header-inventory fallback fires from cmdRoadmapQuery
// when the bullet parser yields zero entries but the file has
// ##/### headings. Source-grep on the bounded bullets-mode slice.
TEST(mcp_roadmap_unrecognised_format, Inv4HeaderInventoryFallback) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty())
        << "INV-4: remotecontrol.cpp not readable";

    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapQuery(",
        "QJsonDocument RemoteControl::cmdRoadmapLog(");
    ASSERT_FALSE(body.empty())
        << "INV-4: failed to bound cmdRoadmapQuery body";

    expect(contains(body, "buildHeaderInventoryEnvelope"),
           "INV-4 / HI-3: bullets-mode fallback dispatches to "
           "buildHeaderInventoryEnvelope");
    expect(contains(body, "header_inventory_fallback")
               || contains(cpp, "header_inventory_fallback"),
           "INV-4 / HI-3: header_inventory_fallback literal "
           "present (in fallback envelope or its emitter)");
    // HI-5: the truly-opaque refusal arm still emits
    // unrecognised_format alongside the fallback.
    expect(contains(body, "\"unrecognised_format\""),
           "INV-4 / HI-5: opaque-file refusal arm preserved");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1463 — every unrecognised_format envelope across the four
// refusal sites carries the shared kUnrecognisedFormatHint +
// expected_format[] fields. Wording regression-locks the hint
// to mention both GFM-task-list and Ants-v1 signatures (and the
// 📋 emoji byte sequence) so a rewording drops the right tests.
TEST(mcp_roadmap_unrecognised_format, Inv5ExpectedFormatField) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty())
        << "INV-5: remotecontrol.cpp not readable";

    // Constant is declared by accessor returning const QString&.
    expect(contains(cpp,
               "const QString &kUnrecognisedFormatHint()"),
           "EF-2: kUnrecognisedFormatHint accessor present");

    // Hint wording carries both bullet-format signatures.
    expect(contains(cpp, "GFM-task-list"),
           "EF-2 wording: GFM-task-list signature present");
    expect(contains(cpp, "Ants-v1"),
           "EF-2 wording: Ants-v1 signature present");
    expect(contains(cpp, "- [ ]") && contains(cpp, "- [x]"),
           "EF-2 wording: GFM bullet signatures `- [ ]` / "
           "`- [x]` present");
    expect(contains(cpp, "\\xF0\\x9F\\x93\\x8B"),
           "EF-2 wording: ants-v1 planned-bullet emoji (📋) "
           "byte sequence present");

    // Four refusal sites all set env["expected_format"] via the
    // shared accessor — count 4 + 1 emitter call inside
    // buildHeaderInventoryEnvelope = 5 occurrences total.
    auto countOccurrences = [&](const std::string &needle) {
        size_t n = 0;
        size_t p = cpp.find(needle);
        while (p != std::string::npos) {
            ++n;
            p = cpp.find(needle, p + needle.size());
        }
        return n;
    };
    const size_t hits =
        countOccurrences("env[\"expected_format\"]");
    expect(hits >= 4,
           "EF-1: env[\"expected_format\"] assigned at "
           "≥ 4 sites");
    EXPECT_EQ(0, expect_failures());
}
