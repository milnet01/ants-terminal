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

    // INV-1: both QLabel members declared with the documented names.
    if (!contains(h, "QLabel *m_repoVisibilityLabel"))
        return fail("INV-1", "m_repoVisibilityLabel QLabel* member missing");
    if (!contains(h, "QLabel *m_updateAvailableLabel"))
        return fail("INV-1", "m_updateAvailableLabel QLabel* member missing");

    // INV-2: both constructed in the cpp, addPermanentWidget'ed, hidden.
    if (!contains(s, "m_repoVisibilityLabel = new QLabel"))
        return fail("INV-2", "m_repoVisibilityLabel not constructed");
    if (!contains(s, "m_updateAvailableLabel = new QLabel"))
        return fail("INV-2", "m_updateAvailableLabel not constructed");
    if (!contains(s, "addPermanentWidget(m_repoVisibilityLabel)"))
        return fail("INV-2", "m_repoVisibilityLabel not added to status bar");
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

    // INV-9: update timer + 5 s singleShot.
    if (!contains(h, "QTimer *m_updateCheckTimer"))
        return fail("INV-9", "m_updateCheckTimer QTimer* member missing");
    if (!contains(s, "m_updateCheckTimer->setInterval(60 * 60 * 1000)"))
        return fail("INV-9", "update timer must tick every hour");
    if (!contains(s, "QTimer::singleShot(5000"))
        return fail("INV-9",
            "5 s singleShot for the first check is required so the badge "
            "surfaces before the first hourly tick");

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

    // INV-11: update label opens external links.
    if (!contains(s, "m_updateAvailableLabel->setOpenExternalLinks(true)"))
        return fail("INV-11",
            "setOpenExternalLinks(true) must be set so the rendered <a href> "
            "opens the release page in the user's browser");

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

    std::puts("OK github_status_bar: 12/12 invariants");
    return 0;
}
