// ANTS-1429 — feature-conformance test for the unrecognised_format
// gate on cmdRoadmapQuery + cmdRoadmapLog. Source-scrape pattern;
// behavioural surface covered by remote_control_roadmap_query.

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

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
// paths. The bound is still a meaningful runaway-guard ceiling.
std::string boundedBetween(const std::string &cpp,
                           const std::string &startSig,
                           const std::string &endSig,
                           size_t kMaxBound = 24 * 1024) {
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-1: remotecontrol.cpp not readable";

    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapQuery(",
        "QJsonDocument RemoteControl::cmdRoadmapLog(");
    ASSERT_FALSE(body.empty())
        << "INV-1: failed to bound cmdRoadmapQuery body (sig moved "
           "or bound > 16 KB)";

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
    const std::string hdr = slurp(SRC_REMOTECONTROL_H_PATH);
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
    const std::string cpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    ASSERT_FALSE(cpp.empty()) << "INV-3: remotecontrol.cpp not readable";

    const std::string body = boundedBetween(
        cpp,
        "QJsonDocument RemoteControl::cmdRoadmapLog(",
        "QJsonDocument RemoteControl::cmdWorkspaceSearch(");
    ASSERT_FALSE(body.empty())
        << "INV-3: failed to bound cmdRoadmapLog body (sig moved "
           "or bound > 16 KB)";

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
