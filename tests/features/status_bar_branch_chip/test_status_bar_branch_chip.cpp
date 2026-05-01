// Feature-conformance test for tests/features/status_bar_branch_chip/spec.md.
//
// Locks ANTS-1109 — git-branch chip styled to match the repo-
// visibility pill, with green/amber color derived from the branch
// name. Drives the pure helper branchchip::isPrimaryBranch +
// source-greps mainwindow.cpp for the consultation site and the
// shape match against the visibility pill.
//
// Exit 0 = all invariants hold.

#include "branchchip.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main() {
    // INV-1..3: the three primary-branch names return true.
    if (!branchchip::isPrimaryBranch(QStringLiteral("main")))
        return fail("INV-1", "main should be primary");
    if (!branchchip::isPrimaryBranch(QStringLiteral("master")))
        return fail("INV-2", "master should be primary");
    if (!branchchip::isPrimaryBranch(QStringLiteral("trunk")))
        return fail("INV-3", "trunk should be primary");

    // INV-4: a feature branch is not primary.
    if (branchchip::isPrimaryBranch(QStringLiteral("feature/foo")))
        return fail("INV-4", "feature/foo should not be primary");

    // INV-5: empty is not primary.
    if (branchchip::isPrimaryBranch(QStringLiteral("")))
        return fail("INV-5", "empty branch should not be primary");

    // INV-6: case-sensitive match — MAIN is not the same as main.
    if (branchchip::isPrimaryBranch(QStringLiteral("MAIN")))
        return fail("INV-6", "MAIN should not be primary (case-sensitive)");

    // INV-7: mainwindow.cpp consults the helper.
    const std::string source = slurp(MAINWINDOW_CPP);
    if (source.empty()) return fail("setup", "mainwindow.cpp not readable");
    if (!contains(source, "branchchip::isPrimaryBranch"))
        return fail("INV-7",
            "mainwindow.cpp does not call branchchip::isPrimaryBranch");

    // INV-8: every branch-chip setStyleSheet site is wired off
    // branchchip::isPrimaryBranch and uses theme.ansi[2] /
    // theme.ansi[3] (the palette roles the visibility pill picks).
    // Re-targeted from the original "shape substrings" check
    // because today's branch-chip stylesheet *already* contains
    // the shape literals (cold-eyes F8). Scans ALL setStyleSheet
    // matches because the chip is restyled from two paths —
    // applyTheme (theme switch) and updateStatusBar (per-poll
    // refresh on branch change). A future regression that drops
    // the helper from one path while keeping the other would
    // silently break the user-visible cue (cold-eyes F1).
    {
        const std::string needle = "m_statusGitBranch->setStyleSheet(";
        size_t pos = 0;
        int sites = 0;
        while ((pos = source.find(needle, pos)) != std::string::npos) {
            ++sites;
            const size_t windowStart = pos > 400 ? pos - 400 : 0;
            const std::string region = source.substr(windowStart,
                pos - windowStart + 600);
            if (!contains(region, "branchchip::isPrimaryBranch"))
                return fail("INV-8",
                    "branch chip setStyleSheet site does not consult branchchip::isPrimaryBranch");
            if (!contains(region, "ansi[2]"))
                return fail("INV-8",
                    "branch chip setStyleSheet site does not reference theme.ansi[2] (primary green)");
            if (!contains(region, "ansi[3]"))
                return fail("INV-8",
                    "branch chip setStyleSheet site does not reference theme.ansi[3] (feature amber)");
            pos += needle.size();
        }
        if (sites < 2)
            return fail("INV-8",
                "expected ≥ 2 branch-chip setStyleSheet sites (applyTheme + updateStatusBar restyle)");
    }

    std::fprintf(stderr,
        "OK — status bar branch chip styling INVs hold.\n");
    return 0;
}
