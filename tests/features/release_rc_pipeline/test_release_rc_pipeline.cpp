// Feature-conformance test for tests/features/release_rc_pipeline/spec.md.
//
// ANTS-1318 frozen-RC pipeline: source-scrape of .github/workflows/
// release.yml + packaging/cut-rc.sh for the INV-5 / INV-8 / INV-3 /
// §4.4 invariants. Behavioural cut verification is manual (§10).
//
// Exit 0 = all invariants hold.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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

    // promote must NOT mark prerelease (it's the public release). Scope to
    // before cmd_cycle() (ANTS-2164 added subcommands after promote).
    const std::string promote = from_marker(sh, "cmd_promote()");
    const std::string promote_body =
        promote.substr(0, promote.find("\ncmd_cycle()"));
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
    // The script must not write version *strings* to bump.json-managed files
    // (/bump owns version edits). ANTS-2164 legitimately rewrites CHANGELOG/
    // metainfo/debian *dates and prose* via awk + atomic rename — never
    // `sed -i`, which keeps this scrape green (ANTS-2164 §8).
    EXPECT_FALSE(has(sh, "sed -i"))
        << "INV-3: cut-rc.sh must not `sed -i`; date/prose rewrites go through "
           "awk + atomic rename, version strings stay with /bump";
}

// ── ANTS-2164 cadence-hardening source-scrape ───────────────────────────────

// The §2.1 helpers exist.
TEST(ReleaseRcPipeline, Ants2164HelpersPresent) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    for (const char *fn : {"unreleased_has_content", "changelog_section_is_placeholder",
                           "release_body_is_placeholder", "roll_unreleased",
                           "stamp_release_date", "rc_age_days",
                           "require_no_version_drift"}) {
        EXPECT_TRUE(has(sh, fn)) << "ANTS-2164 helper missing: " << fn;
    }
}

// INV-5 — the drift check is a HARD gate in all three release paths.
TEST(ReleaseRcPipeline, Ants2164DriftHardGate) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    // The helper aborts non-zero on drift (no warn-and-continue).
    const std::string drift = from_marker(sh, "require_no_version_drift() {");
    EXPECT_TRUE(has(drift, "check-version-drift.sh") &&
                has(drift.substr(0, drift.find('\n', drift.find("check-version-drift.sh"))
                                       + 200), "exit 1"))
        << "INV-5: require_no_version_drift must run check-version-drift.sh and abort";
    for (const char *fn : {"cmd_new_rc()", "cmd_promote()"}) {
        const std::string body = from_marker(sh, fn);
        EXPECT_TRUE(has(body.substr(0, 1200), "require_no_version_drift"))
            << "INV-5: " << fn << " must call require_no_version_drift";
    }
}

// INV-6 — promote tags the frozen RC commit, never main/HEAD.
TEST(ReleaseRcPipeline, Ants2164PromoteTagsFrozenCommit) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    const std::string promote =
        from_marker(sh, "cmd_promote()").substr(0, 4000);
    EXPECT_TRUE(has(promote, "${rctag}^{commit}"))
        << "INV-6: promote must tag the frozen RC commit";
    EXPECT_FALSE(has(promote, "\"${pub}\" -m \"${inflight}\" HEAD") ||
                 has(promote, "${pub}\" \"main\""))
        << "INV-6: promote must not tag main/HEAD";
}

// INV-1/4/9 wired into new-rc; INV-2/3/8 wired into promote.
TEST(ReleaseRcPipeline, Ants2164GuardsWired) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    const std::string newrc =
        from_marker(sh, "cmd_new_rc()").substr(0, 3500);
    for (const char *needle : {"INV-1", "INV-9", "unreleased_has_content",
                               "roll_unreleased", "ALLOW_EMPTY_RC"})
        EXPECT_TRUE(has(newrc, needle)) << "new-rc missing: " << needle;
    const std::string promote =
        from_marker(sh, "cmd_promote()").substr(0, 4000);
    for (const char *needle : {"INV-2", "INV-8", "stamp_release_date",
                               "changelog_section_is_placeholder",
                               "rc_age_days", "PROMOTE_EMPTY", "FORCE_STALE"})
        EXPECT_TRUE(has(promote, needle)) << "promote missing: " << needle;
}

// INV-7 — the cycle subcommand exists, self-skips, and is dispatched.
TEST(ReleaseRcPipeline, Ants2164CycleSubcommand) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    const std::string cycle =
        from_marker(sh, "cmd_cycle()").substr(0, 1500);
    EXPECT_TRUE(has(cycle, "skip promote") && has(cycle, "skip new-rc"))
        << "INV-7: cycle must self-skip each phase";
    EXPECT_TRUE(has(cycle, "cmd_promote") && has(cycle, "cmd_new_rc"))
        << "INV-7: cycle must run both phases";
    EXPECT_TRUE(has(from_marker(sh, "case \"$SUBCMD\""), "cycle)"))
        << "INV-7: cycle must be dispatched";
}

// ── ANTS-2165 hotfix source-scrape ──────────────────────────────────────────

// INV-3 — hotfix refuses an off-main fix SHA.
TEST(ReleaseRcPipeline, Ants2165HotfixRefusesOffMain) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    EXPECT_TRUE(has(sh, "merge-base --is-ancestor"))
        << "2165 INV-3: hotfix must check the fix is on main";
    const std::string hf = from_marker(sh, "cmd_hotfix()").substr(0, 3000);
    EXPECT_TRUE(has(hf, "fix_not_on_main"))
        << "2165 INV-3: hotfix must refuse with fix_not_on_main";
}

// INV-5 — hotfix publishes a PUBLIC (non-prerelease) tag, drift-gated, push-gated.
TEST(ReleaseRcPipeline, Ants2165HotfixIsPublicNotPrerelease) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    const std::string cont =
        from_marker(sh, "cmd_hotfix_continue()").substr(0, 4000);
    ASSERT_FALSE(cont.empty());
    EXPECT_TRUE(has(cont, "gh release create"))
        << "2165 INV-5: hotfix must publish a release";
    EXPECT_FALSE(has(cont, "--prerelease"))
        << "2165 INV-5: the hotfix release is public, not a prerelease";
    EXPECT_TRUE(has(cont, "require_no_version_drift"))
        << "2165 INV-5: hotfix publish is drift-gated";
    EXPECT_TRUE(has(cont, "DO_PUSH"))
        << "2165 INV-5: hotfix remote actions are --push-gated";
}

// INV-7 — hotfix is out-of-cadence: no wednesday_guard on either phase.
TEST(ReleaseRcPipeline, Ants2165HotfixNoWednesdayGuard) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    const std::string hf =
        from_marker(sh, "cmd_hotfix()");
    const std::string p1 = hf.substr(0, hf.find("cmd_hotfix_continue()"));
    const std::string p2 =
        from_marker(sh, "cmd_hotfix_continue()").substr(0, 4000);
    EXPECT_FALSE(has(p1, "wednesday_guard"))
        << "2165 INV-7: hotfix phase 1 must not run wednesday_guard";
    EXPECT_FALSE(has(p2, "wednesday_guard"))
        << "2165 INV-7: hotfix phase 2 must not run wednesday_guard";
}

// The hotfix subcommand + --continue flag are wired into the arg parser.
TEST(ReleaseRcPipeline, Ants2165HotfixArgWiring) {
    const std::string sh = ants_test::slurpFile(CUT_RC_SH_PATH);
    ASSERT_FALSE(sh.empty());
    EXPECT_TRUE(has(sh, "status|new-rc|respin|promote|cycle|hotfix)"))
        << "2165: hotfix (and cycle) must be recognised subcommands";
    EXPECT_TRUE(has(sh, "--continue)"))
        << "2165: --continue must be a parsed flag";
    EXPECT_TRUE(has(from_marker(sh, "case \"$SUBCMD\""), "hotfix)"))
        << "2165: hotfix must be dispatched";
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
