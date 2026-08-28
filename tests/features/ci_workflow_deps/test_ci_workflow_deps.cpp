// ANTS-4391 / ANTS-4717 — every environment that runs the test suite must
// declare each external tool the suite shells out to with no skip path. See
// spec.md for why this is a static check rather than a better test run:
// locally the tool is always present, so no local execution can catch its
// absence on a runner or in a build VM.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#ifndef SRC_CI_WORKFLOW_PATH
#error "SRC_CI_WORKFLOW_PATH compile definition required"
#endif
#ifndef SRC_RPM_SPEC_PATH
#error "SRC_RPM_SPEC_PATH compile definition required"
#endif
#ifndef SRC_ARCH_PKGBUILD_PATH
#error "SRC_ARCH_PKGBUILD_PATH compile definition required"
#endif
#ifndef SRC_DEBIAN_CONTROL_PATH
#error "SRC_DEBIAN_CONTROL_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// A declaration, not a mention. All four carriers comment with `#`, and the
// RPM spec discusses ripgrep at length in prose — so a substring match over
// the raw bytes would pass on the comment block alone, with the
// BuildRequires line deleted. Whole-line comments only; a trailing comment
// after code is not a shape any of these files uses.
std::string codeLines(const std::string &text) {
    std::istringstream in(text);
    std::string line;
    std::string out;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '#') {
            continue;
        }
        out += line;
        out += '\n';
    }
    return out;
}

}  // namespace

// INV-1 — a tool the suite invokes with no skip path is declared by every
// environment that runs the suite.
TEST(CiWorkflowDeps, RequiredToolsAreDeclared) {
    // {label, path}. Flatpak is absent on purpose: it builds with
    // -DANTS_TESTS=OFF and runs no ctest, so it needs no test-only tool.
    const std::vector<std::vector<std::string>> environments = {
        {"the CI workflow", SRC_CI_WORKFLOW_PATH},
        {"the openSUSE/Fedora RPM spec (%check)", SRC_RPM_SPEC_PATH},
        {"the Arch PKGBUILD (check())", SRC_ARCH_PKGBUILD_PATH},
        {"the Debian control (dh_auto_test)", SRC_DEBIAN_CONTROL_PATH},
    };

    // {package, the binary it provides, why it has no skip path}. The
    // package name is the same on every distro here; a tool whose name
    // diverges needs a per-environment spelling, which nothing needs yet.
    const std::vector<std::vector<std::string>> required = {
        {"ripgrep", "rg",
         "cited_by / workspace_search / co_change_family start it through "
         "rcRunRg() and their tests assert on real results, so an absent rg "
         "is a FAILED test (rg_failed), not a skipped one"},
    };

    for (const auto &env : environments) {
        const std::string body = ants_test::slurpFile(env[1]);
        ASSERT_FALSE(body.empty()) << env[1] << " must be readable";
        const std::string code = codeLines(body);

        for (const auto &row : required) {
            EXPECT_TRUE(has(code, row[0]))
                << env[0] << " does not declare `" << row[0] << "` (provides `"
                << row[1] << "`). " << row[2]
                << ". This omission held main red on 2026-08-14 (ANTS-4391) "
                   "and broke the OBS build on 2026-08-26 (ANTS-4717), both "
                   "while the suite was green locally, because the dev host "
                   "has it.";
        }
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
           "leaving every packaging carrier installing a package nothing needs";
}
