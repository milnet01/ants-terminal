// ANTS-1877 — roadmap_log op:append stable-prefix project hint.
// Source-grep style against cmdRoadmapLogAppend + the new helper.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — file-missing vs unreadable distinguished by code.
TEST(roadmap_log_stable_prefix_hint, Inv1FileMissingVsUnreadable) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "counter_missing"),
           "INV-1: counter_missing code present");
    expect(contains(cpp, "stable_prefix_unsupported"),
           "INV-1: stable_prefix_unsupported code present");
    // Existing counter_read_failed retained for the "unreadable /
    // not-a-number" cases.
    expect(contains(cpp, "counter_read_failed"),
           "INV-1: counter_read_failed retained");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — sniffer helper present.
TEST(roadmap_log_stable_prefix_hint, Inv2SnifferContract) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "rlDetectStablePrefixId"),
           "INV-2: rlDetectStablePrefixId helper present");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — envelope shape.
TEST(roadmap_log_stable_prefix_hint, Inv3StablePrefixEnvelope) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "detected_prefix_example"),
           "INV-3: detected_prefix_example field present");
    expect(contains(cpp, "follow_up") &&
           contains(cpp, "ANTS-1877"),
           "INV-3: follow_up field with ANTS-1877 value present");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — counter_missing hint includes the echo recipe.
TEST(roadmap_log_stable_prefix_hint, Inv4CounterMissingHint) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "echo 0 >"),
           "INV-4: counter_missing hint cites the echo recipe");
    EXPECT_EQ(0, expect_failures());
}
