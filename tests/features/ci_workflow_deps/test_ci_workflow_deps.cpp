// ANTS-4391 — the CI workflow must declare every external tool the test
// suite shells out to with no skip path. See spec.md for why this is a
// static check rather than a better test run: locally the tool is always
// present, so no local execution can catch its absence on the runner.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifndef SRC_CI_WORKFLOW_PATH
#error "SRC_CI_WORKFLOW_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — a tool the suite invokes with no skip path is declared in ci.yml.
TEST(CiWorkflowDeps, RequiredToolsAreDeclared) {
    const std::string wf = ants_test::slurpFile(SRC_CI_WORKFLOW_PATH);
    ASSERT_FALSE(wf.empty()) << ".github/workflows/ci.yml must be readable";

    // {apt package, the binary it provides, why it has no skip path}.
    const std::vector<std::vector<std::string>> required = {
        {"ripgrep", "rg",
         "cited_by / workspace_search / co_change_family start it through "
         "rcRunRg() and their tests assert on real results, so an absent rg "
         "is a FAILED test (rg_failed), not a skipped one"},
    };

    for (const auto &row : required) {
        EXPECT_TRUE(has(wf, row[0]))
            << "ci.yml does not install `" << row[0] << "` (provides `"
            << row[1] << "`). " << row[2]
            << ". This exact omission held main red on 2026-08-14 while the "
               "suite was green locally, because the dev host has it.";
    }
}

// The REQUIRED set is only honest if `rg` really is invoked with no skip
// path. If the production code stops shelling out to it, this test is
// pinning a dependency the project no longer has — so tie the assertion to
// the source rather than to a comment that can rot.
TEST(CiWorkflowDeps, RipgrepIsStillInvokedByTheSource) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());
    EXPECT_TRUE(has(rc, "QStringLiteral(\"rg\")"))
        << "no RemoteControl TU starts `rg` any more — if that is deliberate, "
           "drop ripgrep from this test's REQUIRED set as well, rather than "
           "leaving ci.yml installing a package nothing needs";
}
