// Feature-conformance test for tests/features/release_rc_pipeline/spec.md.
//
// ANTS-1318 frozen-RC pipeline: source-scrape of .github/workflows/
// release.yml + packaging/cut-rc.sh for the INV-5 / INV-8 / INV-3 /
// §4.4 invariants. Behavioural cut verification is manual (§10).
//
// Exit 0 = all invariants hold.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_RELEASE_WORKFLOW_PATH
#error "SRC_RELEASE_WORKFLOW_PATH compile definition required"
#endif
#ifndef CUT_RC_SH_PATH
#error "CUT_RC_SH_PATH compile definition required"
#endif

namespace {


bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Substring of `s` from the first occurrence of `from` to end.
std::string from_marker(const std::string &s, const std::string &from) {
    const auto p = s.find(from);
    return p == std::string::npos ? std::string{} : s.substr(p);
}

}  // namespace

// INV-1 — release.yml RC channel split (INV-8).
TEST(ReleaseRcPipeline, Inv1ChannelSplit) {
    const std::string yml = ants_test::slurpFile(SRC_RELEASE_WORKFLOW_PATH);
    ASSERT_FALSE(yml.empty()) << "release.yml not readable";
    EXPECT_TRUE(has(yml, "-rc[0-9]+$"))
        << "INV-8: release.yml must detect RC tags via -rc[0-9]+$";
    EXPECT_TRUE(has(yml, "update_channel"))
        << "INV-8: AppImage update channel must be computed, not fixed";
    EXPECT_TRUE(has(yml,
        "gh-releases-zsync|milnet01|ants-terminal|"
        "${{ steps.tag.outputs.update_channel }}|"))
        << "INV-8: UPDATE_INFORMATION must use the computed channel, "
           "not a hardcoded `latest`";
    // The stable branch still pins latest; the RC branch must not.
    EXPECT_TRUE(has(yml, "UPDATE_CHANNEL=\"latest\""))
        << "INV-8: stable builds keep the latest channel";
    EXPECT_TRUE(has(yml, "UPDATE_CHANNEL=\"${REF}\""))
        << "INV-8: RC builds track their own rc ref, not latest";
}

// INV-2 — release.yml prerelease backstop on auto-create (INV-5).
TEST(ReleaseRcPipeline, Inv2PrereleaseBackstop) {
    const std::string yml = ants_test::slurpFile(SRC_RELEASE_WORKFLOW_PATH);
    ASSERT_FALSE(yml.empty());
    EXPECT_TRUE(has(yml, "PRERELEASE_FLAG=\"--prerelease\""))
        << "INV-5: auto-create path must mark RC releases prerelease";
    EXPECT_TRUE(has(yml, "steps.tag.outputs.is_rc"))
        << "INV-5: prerelease flag must be gated on the is_rc output";
}

// INV-3 — cut-rc.sh prerelease mapping per subcommand (INV-5).
TEST(ReleaseRcPipeline, Inv3CutRcPrereleaseMapping) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty()) << "cut-rc.sh not readable";

    // new-rc and respin create prereleases.
    const std::string newrc = from_marker(sh, "cmd_new_rc()");
    ASSERT_FALSE(newrc.empty());
    // Scope new-rc to before respin starts.
    const std::string newrc_body =
        newrc.substr(0, newrc.find("cmd_respin()"));
    EXPECT_TRUE(has(newrc_body, "--prerelease"))
        << "INV-5: new-rc must create a prerelease";

    const std::string respin = from_marker(sh, "cmd_respin()");
    const std::string respin_body =
        respin.substr(0, respin.find("cmd_promote()"));
    EXPECT_TRUE(has(respin_body, "--prerelease"))
        << "INV-5: respin must create a prerelease";

    // promote must NOT mark prerelease (it's the public release).
    const std::string promote = from_marker(sh, "cmd_promote()");
    const std::string promote_body =
        promote.substr(0, promote.find("\ncase "));
    ASSERT_FALSE(promote_body.empty());
    EXPECT_TRUE(has(promote_body, "gh release create"))
        << "promote must create the public release";
    EXPECT_FALSE(has(promote_body, "--prerelease"))
        << "INV-5: promote must NOT mark the public release prerelease";
}

// INV-4 — cut-rc.sh reads base from CMakeLists, never writes versions.
TEST(ReleaseRcPipeline, Inv4BaseReadOnly) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    EXPECT_TRUE(has(sh, "base_version()") &&
                has(sh, "CMakeLists.txt"))
        << "INV-3: cut-rc.sh derives the base from CMakeLists.txt";
    // The script must not edit version-bearing files.
    EXPECT_FALSE(has(sh, "sed -i"))
        << "INV-3: cut-rc.sh must not rewrite files (suffix lives only "
           "at the tag); /bump owns version edits";
}

// INV-5 — one-RC-in-flight guard (§4.4).
TEST(ReleaseRcPipeline, Inv5OneRcInFlightGuard) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    EXPECT_TRUE(has(sh, "inflight_base"))
        << "§4.4: cut-rc.sh must track the in-flight RC base";
    const std::string newrc = from_marker(sh, "cmd_new_rc()");
    EXPECT_TRUE(has(newrc, "in flight"))
        << "§4.4: new-rc must refuse a second in-flight RC base";
}

// INV-6 — irreversible actions gated behind --push.
TEST(ReleaseRcPipeline, Inv6PushGated) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    EXPECT_TRUE(has(sh, "DO_PUSH") && has(sh, "--push"))
        << "INV-6: pushes/releases must be gated behind --push";
    EXPECT_TRUE(has(sh, "[rehearsal]"))
        << "INV-6: without --push the script must rehearse";
}
