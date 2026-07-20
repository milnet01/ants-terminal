// ANTS-3567 — feature-conformance test: cmdIndieReviewPartition emits a
// sparse_partition_hint pointing at indie_review_brief's source_paths[] ad-hoc
// mode (ANTS-3375) + the .indie-review/partition.json override, mirroring
// cmdColdEyesPartition's ANTS-1634a hint. Source-scrape harness (the wrapper
// needs a MainWindow, so it is greped like the sibling mcp_cold_eyes test).

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

ANTS_TEST_SCOPE();

namespace {

// Scope a lookup to a named function body: from its definition to the next
// top-level `\n}\n` (matches the mcp_cold_eyes FoldIn test's approach).
std::string bodyOf(const std::string &src, const std::string &fnMarker) {
    const auto pos = src.find(fnMarker);
    if (pos == std::string::npos) return {};
    const auto end = src.find("\n}\n", pos);
    if (end == std::string::npos) return src.substr(pos);
    return src.substr(pos, end - pos);
}

}  // namespace

// INV-1/2 — the hint lives inside cmdIndieReviewPartition and names both the
// source_paths[] ad-hoc escape hatch (ANTS-3375) and the override file.
TEST(indie_review_partition_sparse_hint, HintMentionsAdhocAndOverride) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string body =
        bodyOf(rc, "RemoteControl::cmdIndieReviewPartition");
    expect(!body.empty(),
           "cmdIndieReviewPartition body present in remotecontrol.cpp");
    expect(body.find("sparse_partition_hint") != std::string::npos,
           "INV: cmdIndieReviewPartition emits sparse_partition_hint");
    expect(body.find("ANTS-3567") != std::string::npos,
           "INV: ANTS-3567 anchor present");
    expect(body.find("source_paths") != std::string::npos,
           "INV-1: hint points at the source_paths[] ad-hoc mode");
    expect(body.find("ANTS-3375") != std::string::npos,
           "INV-1: hint cites ANTS-3375");
    expect(body.find(".indie-review/partition.json") != std::string::npos,
           "INV-2: hint points at the override file");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — the hint is gated on a ≤1-lane partition (the sparse case), mirroring
// cmdColdEyesPartition's `lanes.size() <= 1` gate, not emitted unconditionally.
TEST(indie_review_partition_sparse_hint, GatedOnSparsePartition) {
    expect_reset();
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string body =
        bodyOf(rc, "RemoteControl::cmdIndieReviewPartition");
    expect(!body.empty(),
           "cmdIndieReviewPartition body present");
    expect(body.find("lanes.size() <= 1") != std::string::npos,
           "INV-3: hint gated on a ≤1-lane (sparse) partition");
    expect(body.find("sparse_partition") != std::string::npos,
           "INV-3: sparse_partition boolean flag emitted alongside the hint");
    EXPECT_EQ(0, expect_failures());
}
