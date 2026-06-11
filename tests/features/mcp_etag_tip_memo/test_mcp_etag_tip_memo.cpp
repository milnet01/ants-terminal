// ANTS-1568 — feature-conformance test for the "Etag tip" memo that
// the tools/list post-processing loop appends to every etag-supporting
// tool's description. Source-scrape against claudeintegration.cpp.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — the post-processing loop at the ANTS-1518 prefix-injection
// site appends an "Etag tip:" memo to every etag-supporting tool.
TEST(mcp_etag_tip_memo, Inv1AppendBlockPresent) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "ANTS-1568"),
           "INV-1: ANTS-1568 anchor present in the post-processing "
           "block (same site as ANTS-1518 prefix injection)");
    expect(contains(ci, "isEtagSupportedTool(name)") &&
           contains(ci, "Etag tip:"),
           "INV-1: append block guards on isEtagSupportedTool(name) "
           "and emits the literal 'Etag tip:' memo");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — the memo is gated on isEtagSupportedTool, so non-etag
// tools don't get it. Asserts the conditional structure (not the
// negative emission against every tool).
TEST(mcp_etag_tip_memo, Inv2GatedOnIsEtagSupportedTool) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // The block reads:  if (isEtagSupportedTool(name) && ... ) { ... }
    // — both halves of the && must appear together.
    expect(contains(ci, "isEtagSupportedTool(name) &&"),
           "INV-2: append is conditional on isEtagSupportedTool — "
           "non-etag tools don't get the memo");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — idempotent. The append guard checks for the literal
// "Etag tip:" substring before appending so a hot-reload or
// repeated tools/list call doesn't double-append.
TEST(mcp_etag_tip_memo, Inv3IdempotentSentinel) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(ci, "!desc.contains(QStringLiteral(\"Etag tip:\"))"),
           "INV-3: guard checks !desc.contains(\"Etag tip:\") so the "
           "append is idempotent across tools/list reissues");
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — eight etag-supporting tools — sanity check that
// isEtagSupportedTool's whitelist still names all eight (the memo
// is meaningless if a tool was dropped from the whitelist).
TEST(mcp_etag_tip_memo, Inv4EtagSupportedToolWhitelistIntact) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const char *expected[] = {
        "\"project_layout\"", "\"roadmap_query\"", "\"file_outline\"",
        "\"last_audit_summary\"", "\"get_environment\"",
        "\"tab_list\"", "\"subsystem\"", "\"git_state\""
    };
    for (const char *needle : expected) {
        expect(contains(ci, needle),
               (std::string("INV-4: ") + needle +
                " still named in source (etag whitelist)").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}
