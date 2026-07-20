// ANTS-3583 — feature-conformance test for roadmap_query prose-roadmap
// warning parity across status filters. Source-scrape harness (matching the
// sibling roadmap_query_narrator_filter / mcp_roadmap_status_filter tests);
// no GUI, no Roadmap fixture.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — on the empty-result path the handler scans the whole cached
// bullet set (file-level, filter-independent) for any id-bearing bullet.
TEST(roadmap_query_prose_no_id_warning, Inv1FileLevelIdBearingScan) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "ANTS-3583"),
           "INV-1: ANTS-3583 anchor present in remotecontrol.cpp");
    expect(contains(cpp, "fileHasIdBearingBullet"),
           "INV-1: file-level id-bearing scan flag present");
    // The scan iterates the cached bullet set through the same drop helper
    // the emission loop uses, so 'id-bearing' means the same thing here.
    expect(contains(cpp, "std::as_const(m_roadmapCacheBullets)"),
           "INV-1: scan iterates the whole cached bullet set");
    expect(contains(cpp, "fileHasIdBearingBullet = true; break;"),
           "INV-1: early-exit on the first id-bearing bullet keeps it O(1)");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — zero id-bearing bullets => parseable_bullets:0 + a "format not
// recognised" warning, on every status filter.
TEST(roadmap_query_prose_no_id_warning, Inv2ParseableBulletsSignalAndWarning) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "out[\"parseable_bullets\"] = 0"),
           "INV-2: machine-detectable parseable_bullets:0 emitted");
    expect(contains(cpp, "no [PROJ-NNNN]-tagged bullets that "),
           "INV-2: warning names the unrecognised-format cause");
    expect(contains(cpp, "NOT \\\"no outstanding "),
           "INV-2: warning explicitly disclaims the 'no work left' reading");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — filter-independence: the new branch is gated on the file-level
// scan + the two opt-in flags, NOT on the post-status preIdPruneCountFull
// (the value the status filter was able to zero). It must precede the
// ANTS-1538 preIdPruneCountFull branch.
TEST(roadmap_query_prose_no_id_warning, Inv3GatedOnFileScanNotPostStatusCount) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(contains(cpp, "if (!fileHasIdBearingBullet &&"),
           "INV-3: branch gated on the file-level scan (not the post-status "
           "count)");
    // Ordering: the ANTS-3583 parseable_bullets branch must appear before the
    // ANTS-1538 preIdPruneCountFull branch so it supersedes it for the
    // prose-roadmap case on status:'all' too.
    const std::string::size_type parseablePos =
        cpp.find("out[\"parseable_bullets\"] = 0");
    const std::string::size_type preIdPrunePos =
        cpp.find("} else if (preIdPruneCountFull > 0 &&");
    expect(parseablePos != std::string::npos &&
           preIdPrunePos != std::string::npos &&
           parseablePos < preIdPrunePos,
           "INV-3: parseable_bullets branch precedes the preIdPruneCountFull "
           "branch");
    EXPECT_EQ(0, expect_failures());
}
