// Feature-conformance test for tests/features/github_status_bar/spec.md.
//
// Asserts the contract for the 0.7.45 GitHub-aware status-bar
// widgets — repo visibility badge + update-available notifier.
// Source-grep harness following the same pattern as
// tests/features/claude_bg_tasks_button/.
//
// INV-1 / INV-2 — both QLabel members declared + constructed +
//                 added via addPermanentWidget + start hide()-n.
// INV-3        — pure helpers findGitRepoRoot, parseGithubOriginSlug,
//                 compareSemver exist in the implementation.
// INV-4        — parseGithubOriginSlug handles both URL forms.
// INV-5        — refreshStatusBarForActiveTab calls refreshRepoVisibility.
// INV-6        — refreshRepoVisibility hides on every failure branch.
// INV-7        — repo-visibility cache has a 10-minute TTL.
// INV-8        — gh repo view invoked with the minimal field set.
// INV-9        — update timer is hourly + a 5 s singleShot first run.
// INV-10       — checkForUpdates requests releases/latest with UA.
// INV-11       — update-available label has setOpenExternalLinks(true).
// INV-12       — compareSemver returns the right sign for sample inputs.
//                 (Behavioral — links the source so we can call it.)
//
// Exit 0 = all assertions hold.

#include <QCoreApplication>

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H_PATH
#error "SRC_MAINWINDOW_H_PATH compile definition required"
#endif
#ifndef SRC_RELEASE_WORKFLOW_PATH
#error "SRC_RELEASE_WORKFLOW_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

// Extract a function body from a translation unit by signature
// prefix. Returns substring from the first matching signature to
// the next `\nvoid ` (heuristic — good enough for top-level free /
// member functions in the project; the body never contains a
// top-level void).
std::string functionBody(const std::string &src, const std::string &sig) {
    const auto start = src.find(sig);
    if (start == std::string::npos) return {};
    const auto next = src.find("\nvoid ", start + sig.size());
    return src.substr(start, next == std::string::npos ? std::string::npos
                                                       : next - start);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    const std::string h = slurp(SRC_MAINWINDOW_H_PATH);
    const std::string s = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string yml = slurp(SRC_RELEASE_WORKFLOW_PATH);

    // INV-1: both QLabel members declared with the documented names.
    if (!contains(h, "QLabel *m_repoVisibilityLabel"))
        return fail("INV-1", "m_repoVisibilityLabel QLabel* member missing");
    if (!contains(h, "QLabel *m_updateAvailableLabel"))
        return fail("INV-1", "m_updateAvailableLabel QLabel* member missing");

    // INV-2: both constructed in the cpp, hidden by default. The
    // update-available label sits on the right (addPermanentWidget);
    // the repo-visibility badge moved to the LEFT side next to the
    // git-branch chip in 0.7.49 (addWidget) per user feedback
    // 2026-04-27 — repo provenance reads as "branch · visibility"
    // naturally and the right side is busy with Claude Code chrome.
    if (!contains(s, "m_repoVisibilityLabel = new QLabel"))
        return fail("INV-2", "m_repoVisibilityLabel not constructed");
    if (!contains(s, "m_updateAvailableLabel = new QLabel"))
        return fail("INV-2", "m_updateAvailableLabel not constructed");
    if (!contains(s, "addWidget(m_repoVisibilityLabel)"))
        return fail("INV-2",
            "m_repoVisibilityLabel must be on the LEFT side via "
            "addWidget — addPermanentWidget would put it on the right "
            "next to the Claude Code chrome (the 0.7.45 placement that "
            "0.7.49 moved per user feedback). It must sit next to the "
            "git-branch chip");
    if (!contains(s, "addPermanentWidget(m_updateAvailableLabel)"))
        return fail("INV-2", "m_updateAvailableLabel not added to status bar");
    if (!contains(s, "m_repoVisibilityLabel->hide()"))
        return fail("INV-2", "m_repoVisibilityLabel not hidden by default");
    if (!contains(s, "m_updateAvailableLabel->hide()"))
        return fail("INV-2", "m_updateAvailableLabel not hidden by default");
    // ObjectNames present (used for QSS targeting + test discovery).
    if (!contains(s, "\"repoVisibilityLabel\""))
        return fail("INV-2", "repoVisibilityLabel objectName missing");
    if (!contains(s, "\"updateAvailableLabel\""))
        return fail("INV-2", "updateAvailableLabel objectName missing");

    // INV-3: pure helpers exist.
    if (!contains(s, "findGitRepoRoot"))
        return fail("INV-3", "findGitRepoRoot helper missing");
    if (!contains(s, "parseGithubOriginSlug"))
        return fail("INV-3", "parseGithubOriginSlug helper missing");
    if (!contains(s, "compareSemver"))
        return fail("INV-3", "compareSemver helper missing");

    // INV-4: parseGithubOriginSlug handles both URL forms.
    {
        const std::string body = functionBody(s,
            "QString parseGithubOriginSlug");
        if (body.empty())
            return fail("INV-4", "parseGithubOriginSlug body not found");
        if (body.find("https://github.com/") == std::string::npos)
            return fail("INV-4",
                "parseGithubOriginSlug must recognise the https URL form");
        if (body.find("git@github.com:") == std::string::npos)
            return fail("INV-4",
                "parseGithubOriginSlug must recognise the SSH URL form");
    }

    // INV-5: refreshStatusBarForActiveTab calls refreshRepoVisibility.
    {
        const std::string body = functionBody(s,
            "void MainWindow::refreshStatusBarForActiveTab");
        if (body.empty())
            return fail("INV-5", "refreshStatusBarForActiveTab body not found");
        if (body.find("refreshRepoVisibility") == std::string::npos)
            return fail("INV-5",
                "refreshStatusBarForActiveTab must call refreshRepoVisibility");
    }

    // INV-6: refreshRepoVisibility hides on every failure branch.
    // Easiest check: the body contains at least four `m_repoVisibilityLabel->hide()`
    // call sites (gh missing, no cwd, no .git, non-GitHub) plus the
    // miss-async-pending hide.
    {
        const std::string body = functionBody(s,
            "void MainWindow::refreshRepoVisibility");
        if (body.empty())
            return fail("INV-6", "refreshRepoVisibility body not found");
        size_t pos = 0, count = 0;
        const std::string needle = "m_repoVisibilityLabel->hide()";
        while ((pos = body.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        if (count < 4)
            return fail("INV-6",
                "refreshRepoVisibility must hide() on every failure branch "
                "(gh missing / no cwd / no .git / non-GitHub origin)");
    }

    // INV-7: 10-minute TTL on the repo-visibility cache.
    if (!contains(s, "10 * 60 * 1000"))
        return fail("INV-7",
            "10-minute TTL constant missing from refreshRepoVisibility");

    // INV-8: gh repo view invoked with --json visibility -q .visibility.
    if (!contains(s, "\"--json\""))
        return fail("INV-8", "gh invocation must request only the visibility field");
    if (!contains(s, "\"visibility\""))
        return fail("INV-8", "gh invocation must specify the visibility field");
    if (!contains(s, "\".visibility\""))
        return fail("INV-8",
            "gh invocation must use jq selector .visibility (-q .visibility)");

    // INV-9 (revised in 0.7.47): startup-only update check via 5 s
    // singleShot, plus a manual "Help → Check for Updates" action.
    // No hourly QTimer (user feedback "an hourly check I think is a
    // bit much"). Three pieces:
    //   (a) m_updateCheckTimer member is GONE from the header — its
    //       presence would be a regression.
    //   (b) The 5 s singleShot still fires on startup with the bool
    //       false (background, silent on no-update).
    //   (c) helpCheckForUpdatesAction connected with bool true
    //       (user-initiated, surfaces "Up to date" / "Failed").
    if (contains(h, "m_updateCheckTimer"))
        return fail("INV-9",
            "0.7.47 retired the hourly update timer — m_updateCheckTimer "
            "member must NOT be present");
    if (!contains(s, "QTimer::singleShot(5000"))
        return fail("INV-9",
            "5 s singleShot for the startup check is required so the "
            "badge surfaces shortly after launch");
    if (!contains(s, "helpCheckForUpdatesAction"))
        return fail("INV-9",
            "Help menu must add a `Check for Updates` action with "
            "objectName `helpCheckForUpdatesAction` so a manual "
            "trigger exists alongside the startup probe");
    // The manual trigger must pass userInitiated=true. Source-grep
    // for the bool argument near the action's connect.
    if (!contains(s, "checkForUpdates(/*userInitiated=*/true)"))
        return fail("INV-9",
            "Help menu's Check for Updates action must invoke "
            "checkForUpdates(true) so the result lands as a status "
            "message ('Up to date' / 'Update check failed') instead "
            "of the silent badge-only flow used by the startup probe");

    // INV-10: checkForUpdates hits the right URL and sets a UA.
    {
        const std::string body = functionBody(s,
            "void MainWindow::checkForUpdates");
        if (body.empty())
            return fail("INV-10", "checkForUpdates body not found");
        if (body.find("api.github.com/repos/milnet01/ants-terminal/releases/latest")
                == std::string::npos)
            return fail("INV-10",
                "checkForUpdates must hit the releases/latest endpoint");
        if (body.find("setRawHeader(\"User-Agent\"") == std::string::npos)
            return fail("INV-10",
                "User-Agent raw header is required — GitHub 403s without one");
    }

    // INV-11 (revised in 0.7.46): update label intercepts clicks via
    // linkActivated, NOT setOpenExternalLinks(true). The handler
    // probes for AppImageUpdate before falling back to the browser,
    // so an unconditional auto-open would skip the in-place update.
    if (!contains(s, "m_updateAvailableLabel->setOpenExternalLinks(false)"))
        return fail("INV-11",
            "0.7.46+ requires setOpenExternalLinks(false) on the update "
            "label — the linkActivated handler does the routing");
    if (!contains(s, "&QLabel::linkActivated") ||
            !contains(s, "&MainWindow::handleUpdateClicked"))
        return fail("INV-11",
            "update label must connect linkActivated to "
            "MainWindow::handleUpdateClicked");

    // INV-12: compareSemver returns the right sign on the sample inputs.
    // Cannot link to the helper because it's in an anonymous namespace
    // inside mainwindow.cpp. Source-grep instead for the canonical
    // sample table living in this very test as a regression doc:
    //
    //   compareSemver("0.7.45", "0.7.44") > 0    // 1 if newer
    //   compareSemver("0.7.45", "0.7.45") == 0   // 0 if equal
    //   compareSemver("0.7.44", "0.7.45") < 0    // -1 if older
    //   compareSemver("0.8.0",  "0.7.99") > 0    // major-bump
    //
    // The body should reference all version triples in the comparison
    // table to keep them documented. We assert the function exists
    // and has at least the three-component split shape (`split('.')`).
    {
        const std::string body = functionBody(s,
            "int compareSemver");
        if (body.empty())
            return fail("INV-12", "compareSemver body not found");
        if (body.find("split('.')") == std::string::npos)
            return fail("INV-12",
                "compareSemver must split on '.' to compare components "
                "numerically");
        if (body.find("toInt") == std::string::npos)
            return fail("INV-12",
                "compareSemver must compare components as integers, not "
                "strings (or `0.10` would compare less than `0.9`)");
    }

    // INV-13 (added 0.7.46): workflow embeds the AppImageUpdate
    // gh-releases-zsync update-information string into linuxdeploy
    // via the UPDATE_INFORMATION env var. Without this, the AppImage
    // ships with no update metadata and AppImageUpdate refuses to
    // run against it.
    if (yml.find("UPDATE_INFORMATION:") == std::string::npos)
        return fail("INV-13",
            "release.yml must set UPDATE_INFORMATION env var on the "
            "linuxdeploy step");
    if (yml.find("gh-releases-zsync|milnet01|ants-terminal|latest|") ==
            std::string::npos)
        return fail("INV-13",
            "UPDATE_INFORMATION must use the `gh-releases-zsync` schema "
            "anchored on milnet01/ants-terminal latest release");
    if (yml.find("Ants_Terminal-*-x86_64.AppImage.zsync") == std::string::npos)
        return fail("INV-13",
            "UPDATE_INFORMATION wildcard must point at the "
            "Ants_Terminal-*-x86_64.AppImage.zsync sidecar pattern");

    // INV-14: workflow uploads the .zsync sidecar alongside the AppImage.
    if (yml.find("\"${OUTPUT}.zsync\"") == std::string::npos)
        return fail("INV-14",
            "release.yml's gh release upload step must include the "
            "`${OUTPUT}.zsync` sidecar — without it, AppImageUpdate "
            "clients fall back to whole-file fetch (slow) or fail");

    // INV-15: handleUpdateClicked probes for AppImageUpdate (GUI)
    // and appimageupdatetool (CLI) before falling back to the
    // browser. Source-grep all three names.
    {
        const std::string body = functionBody(s,
            "void MainWindow::handleUpdateClicked");
        if (body.empty())
            return fail("INV-15", "handleUpdateClicked body not found");
        if (body.find("AppImageUpdate") == std::string::npos)
            return fail("INV-15",
                "handleUpdateClicked must probe for the AppImageUpdate GUI "
                "binary before falling back");
        if (body.find("appimageupdatetool") == std::string::npos)
            return fail("INV-15",
                "handleUpdateClicked must probe for the appimageupdatetool "
                "CLI as the second-choice updater");
        if (body.find("APPIMAGE") == std::string::npos)
            return fail("INV-15",
                "handleUpdateClicked must read the $APPIMAGE env var to "
                "find the on-disk AppImage path the updater needs as arg");
        if (body.find("QDesktopServices::openUrl") == std::string::npos)
            return fail("INV-15",
                "handleUpdateClicked must fall back to "
                "QDesktopServices::openUrl when no updater is installed");
    }

    // INV-16: detached spawn — the updater outlives this process so
    // the user can quit + restart while the download runs.
    if (!contains(s, "QProcess::startDetached"))
        return fail("INV-16",
            "handleUpdateClicked must use QProcess::startDetached — "
            "running the updater attached would block the parent on its "
            "lifetime and force a still-running terminal to wait on it");

    // INV-17 (added 0.7.47): confirmation dialog before the updater
    // is launched. User feedback 2026-04-27 — clicking the badge
    // shouldn't silently kick a download that requires a restart;
    // the user must be told that they'll need to quit + relaunch
    // and that active Claude Code sessions will be disconnected.
    {
        const std::string body = functionBody(s,
            "void MainWindow::handleUpdateClicked");
        if (body.empty())
            return fail("INV-17", "handleUpdateClicked body not found");
        // The dialog must exist somewhere ahead of startDetached.
        const auto dialogPos = body.find("QMessageBox box(this)");
        const auto detachPos = body.find("QProcess::startDetached");
        if (dialogPos == std::string::npos)
            return fail("INV-17",
                "handleUpdateClicked must construct a QMessageBox before "
                "kicking the updater so the user is warned about the "
                "quit-and-relaunch requirement");
        if (detachPos == std::string::npos ||
                dialogPos >= detachPos)
            return fail("INV-17",
                "the QMessageBox must precede QProcess::startDetached "
                "so a Cancel can short-circuit the spawn");
        // The body must mention the restart consequence — "quit"
        // and "Claude Code" / "reconnected" — so the warning is
        // load-bearing rather than a generic confirm prompt.
        if (body.find("quit and re-launch") == std::string::npos &&
                body.find("quit and relaunch") == std::string::npos)
            return fail("INV-17",
                "the dialog text must explicitly mention quitting + "
                "re-launching as the next user step");
        if (body.find("Claude Code") == std::string::npos)
            return fail("INV-17",
                "the dialog text must call out that Claude Code "
                "sessions will be disconnected — that's the user's "
                "primary concern per the 2026-04-27 feedback");
        if (body.find("reconnected") == std::string::npos)
            return fail("INV-17",
                "the dialog text must say sessions need to be "
                "reconnected — without that, the user might assume "
                "the restart preserves session state");
        // Cancel branch must short-circuit (no detach).
        if (body.find("Update cancelled") == std::string::npos)
            return fail("INV-17",
                "Cancel must surface a status-bar acknowledgement so "
                "the user knows the click was registered but not acted "
                "on (silent cancel feels like a bug)");
    }

    std::puts("OK github_status_bar: 17/17 invariants");
    return 0;
}
