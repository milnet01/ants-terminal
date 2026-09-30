// Feature-conformance test for tests/features/release_pipeline/spec.md.
//
// Why this exists: ANTS-5577 retired the RC cadence. Every release is a full
// public release created by release.yml with its files attached. A leftover RC
// branch in the workflow (prerelease flag, computed update channel, an
// unanchored tag pattern) or a release.sh that creates the release itself
// would bring the old "published before the AppImage is attached" defect back.
//
// Source-scrape of .github/workflows/release.yml and packaging/release.sh
// (INV-17). The behavioural layer is release_behaviour_test.sh and
// obs_status_behaviour_test.sh (registered as ctest release_behaviour and
// obs_status_behaviour).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#ifndef SRC_RELEASE_WORKFLOW_PATH
#error "SRC_RELEASE_WORKFLOW_PATH compile definition required"
#endif
#ifndef RELEASE_SH_PATH
#error "RELEASE_SH_PATH compile definition required"
#endif

namespace {

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Every shell command line starting at an occurrence of `marker`, joined over
// backslash-continued lines, so "the same command" is one string.
std::vector<std::string> commands_starting_with(const std::string &s,
                                                const std::string &marker) {
    std::vector<std::string> out;
    for (auto p = s.find(marker); p != std::string::npos;
         p = s.find(marker, p + 1)) {
        std::string cmd;
        auto q = p;
        while (q < s.size()) {
            const auto nl = s.find('\n', q);
            const auto end = nl == std::string::npos ? s.size() : nl;
            std::string line = s.substr(q, end - q);
            const bool cont = !line.empty() && line.back() == '\\';
            cmd += line + "\n";
            if (!cont || nl == std::string::npos) break;
            q = nl + 1;
        }
        out.push_back(cmd);
    }
    return out;
}

}  // namespace

// INV-17 — release.yml has no RC handling left.
TEST(ReleasePipeline, Inv17WorkflowHasNoRcHandling) {
    const std::string yml = ants_test::slurpFile(SRC_RELEASE_WORKFLOW_PATH);
    ASSERT_FALSE(yml.empty()) << "release.yml not readable";
    for (const char *needle : {"is_rc", "IS_RC", "--prerelease", "update_channel"})
        EXPECT_FALSE(has(yml, needle))
            << "INV-17: release.yml still carries RC handling: " << needle;
}

// INV-17 — the workflow refuses a tag that is not exactly vX.Y.Z.
TEST(ReleasePipeline, Inv17WorkflowRefusesNonReleaseTags) {
    const std::string yml = ants_test::slurpFile(SRC_RELEASE_WORKFLOW_PATH);
    ASSERT_FALSE(yml.empty());
    EXPECT_TRUE(has(yml, "^v[0-9]+\\.[0-9]+\\.[0-9]+$"))
        << "INV-17: release.yml must anchor the tag check with "
           "^v[0-9]+\\.[0-9]+\\.[0-9]+$";
}

// INV-17 — every AppImage points its updater at the latest channel.
TEST(ReleasePipeline, Inv17UpdateInformationIsLatest) {
    const std::string yml = ants_test::slurpFile(SRC_RELEASE_WORKFLOW_PATH);
    ASSERT_FALSE(yml.empty());
    EXPECT_TRUE(has(yml, "gh-releases-zsync|milnet01|ants-terminal|latest|"))
        << "INV-17: UPDATE_INFORMATION must contain the literal "
           "gh-releases-zsync|milnet01|ants-terminal|latest|";
}

// INV-17 — the release is created with its files in one command, notes from
// packaging/release-notes.sh. A create without the files is what published a
// release ~25 minutes before the AppImage was attached.
TEST(ReleasePipeline, Inv17ReleaseCreatedWithItsFiles) {
    const std::string yml = ants_test::slurpFile(SRC_RELEASE_WORKFLOW_PATH);
    ASSERT_FALSE(yml.empty());
    const auto cmds = commands_starting_with(yml, "gh release create");
    ASSERT_FALSE(cmds.empty()) << "INV-17: release.yml must run gh release create";
    for (const auto &c : cmds)
        EXPECT_TRUE(has(c, "\"${UPLOADS[@]}\""))
            << "INV-17: every gh release create must carry the files in the "
               "same command (\"${UPLOADS[@]}\"); got:\n" << c;
    EXPECT_TRUE(has(yml, "packaging/release-notes.sh"))
        << "INV-17: release notes must come from packaging/release-notes.sh";
}

// INV-17 — release.sh: the workflow, not the script, creates the release; no
// in-place edits; no RC or weekday logic; rehearsal is present.
TEST(ReleasePipeline, Inv17ReleaseScriptShape) {
    const std::string sh = ants_test::slurpFile(RELEASE_SH_PATH);
    ASSERT_FALSE(sh.empty()) << "release.sh not readable";
    EXPECT_FALSE(has(sh, "gh release create"))
        << "INV-17: release.sh must not create the release; release.yml does";
    EXPECT_FALSE(has(sh, "sed -i"))
        << "INV-17: release.sh must not `sed -i`; edits go through a temp "
           "file and an atomic rename";
    EXPECT_FALSE(has(sh, "--prerelease"))
        << "INV-17: release.sh must not mark anything prerelease";
    EXPECT_FALSE(has(lower(sh), "wednesday"))
        << "INV-17: release.sh must carry no weekday cadence";
    EXPECT_TRUE(has(sh, "[rehearsal]"))
        << "INV-17: without --push the script must rehearse ([rehearsal])";
}
