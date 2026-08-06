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

// Bounded substring between two signatures. Returns the substring from
// the first signature's position up to the second's, or {} if either
// signature is missing — that match IS the guard: it catches a reorder
// that would otherwise scrape the wrong function body.
//
// ANTS-3633 removed a size ceiling that used to sit here. It was bumped
// eleven times (16→24→28→32→40→44→52→64→72→80→88→96 KiB) and every
// firing was resolved by raising the number, never once by extracting a
// helper — so it never worked as the "think twice about a helper" nudge
// its comment claimed, it just taxed each edit to cmdRoadmapQuery with a
// second, unrelated round trip. Policing function size is a lint concern,
// not something to smuggle into a regex-window helper.
std::string boundedBetween(const std::string &cpp,
                           const std::string &startSig,
                           const std::string &endSig) {
    const auto startPos = cpp.find(startSig);
    if (startPos == std::string::npos) return {};
    const auto endPos = cpp.find(endSig, startPos + startSig.size());
    if (endPos == std::string::npos) return {};
    const auto len = endPos - startPos;
    if (len == 0) return {};
    return cpp.substr(startPos, len);
}

}  // namespace

// INV-1 — cmdRoadmapQuery gate.
TEST(mcp_roadmap_unrecognised_format, Inv1QueryGate) {
    expect_reset();
    const std::string cpp = ants_test::slurpRemoteControl();
    ASSERT_FALSE(cpp.empty()) << "INV-1: remotecontrol.cpp not readable";

    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapQuery(",
        "QJsonDocument RemoteControl::cmdRoadmapLog(");
    ASSERT_FALSE(body.empty())
        << "INV-1: failed to bound cmdRoadmapQuery body (one of the two "
           "bounding signatures moved or was renamed)";

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
    const std::string cpp = ants_test::slurpRemoteControl();
    ASSERT_FALSE(cpp.empty()) << "INV-3: remotecontrol.cpp not readable";

    // The `unrecognised_format` WRITE gate lives in cmdRoadmapLogAppend
    // (cmdRoadmapLog itself is now a thin dispatcher). Bound that function
    // directly — bounding from the dispatcher instead swept up the unrelated
    // cmdChangelog* functions that sit between it and cmdRoadmapLogFlip in the
    // TU, so the assertions below were being checked against a span that
    // was mostly unrelated code (ANTS source-scrape window brittleness).
    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapLogAppend(",
        "QJsonDocument RemoteControl::cmdRoadmapLogFlip(");
    ASSERT_FALSE(body.empty())
        << "INV-3: failed to bound cmdRoadmapLogAppend body (one of the "
           "two bounding signatures moved or was renamed)";

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
    const std::string cpp = ants_test::slurpRemoteControl();
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
    const std::string cpp = ants_test::slurpRemoteControl();
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
