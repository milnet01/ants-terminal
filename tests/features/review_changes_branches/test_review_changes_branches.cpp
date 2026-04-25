// Feature-conformance test for spec.md —
//
// Source-grep test. The Review Changes dialog must run the two
// new branch-aware probes alongside the original three, render
// both new sections, and include them in the Copy Diff payload.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {

int g_failures = 0;

void expect(bool cond, const char *label, const std::string &detail = {}) {
    if (cond) {
        std::fprintf(stderr, "[PASS] %s\n", label);
    } else {
        std::fprintf(stderr, "[FAIL] %s  %s\n", label, detail.c_str());
        ++g_failures;
    }
}

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

int main() {
    const std::string src = slurp(SRC_MAINWINDOW_CPP_PATH);

    // I1 — ProbeState declares the new fields.
    expect(contains(src, "QString branches;"),
           "I1/probestate-branches-field");
    expect(contains(src, "QString crossUnpushed;"),
           "I1/probestate-crossUnpushed-field");
    expect(contains(src, "int pending = 5;"),
           "I1/probestate-pending-counter-is-five",
           "expected `int pending = 5;`");

    // I2 — runAsync calls for both new probes.
    expect(contains(src, "for-each-ref"),
           "I2/runAsync-for-each-ref");
    expect(contains(src, "&ProbeState::branches"),
           "I2/runAsync-targets-branches-slot");
    expect(contains(src, "\"--branches\""),
           "I2/runAsync-log-branches-flag");
    expect(contains(src, "\"--not\""),
           "I2/runAsync-log-not-flag");
    expect(contains(src, "\"--remotes\""),
           "I2/runAsync-log-remotes-flag");
    expect(contains(src, "&ProbeState::crossUnpushed"),
           "I2/runAsync-targets-crossUnpushed-slot");

    // I3 — finalize renders both new sections.
    expect(contains(src, "section(QStringLiteral(\"Branches\"))"),
           "I3/finalizer-emits-branches-section");
    expect(contains(src, "section(QStringLiteral(\"Unpushed across all branches\"))"),
           "I3/finalizer-emits-cross-branch-section");
    expect(contains(src, "section(QStringLiteral(\"Unpushed commits (current branch)\"))"),
           "I3/finalizer-renames-current-branch-section");

    // I4 — Copy handler includes both new sections.
    expect(contains(src, "# Branches\\n"),
           "I4/copy-handler-includes-branches");
    expect(contains(src, "# Unpushed across all branches\\n"),
           "I4/copy-handler-includes-cross-branch");

    // I5 — empty-state guard mentions all 5 fields.
    expect(contains(src, "state->branches.isEmpty()") &&
               contains(src, "state->crossUnpushed.isEmpty()"),
           "I5/empty-state-guard-checks-new-fields");

    // I6 — runProbes lambda exists and reconstructs a fresh
    // ProbeState every call. The shared_ptr<ProbeState> is created
    // *inside* the lambda body, not before it.
    expect(contains(src, "auto runProbes = [this, cwd, dlgGuard"),
           "I6/runProbes-lambda-defined");
    expect(contains(src, "auto state = std::make_shared<ProbeState>();"),
           "I6/runProbes-constructs-fresh-state-per-call");

    // I7 — QFileSystemWatcher armed and connected through a
    // debounce QTimer.
    expect(contains(src,
               "auto *watcher = new QFileSystemWatcher(dialog);"),
           "I7/watcher-constructed-on-dialog");
    expect(contains(src, "auto *debounce = new QTimer(dialog);"),
           "I7/debounce-timer-constructed");
    expect(contains(src, "debounce->setSingleShot(true);"),
           "I7/debounce-is-single-shot");
    expect(contains(src, "debounce->setInterval(300);"),
           "I7/debounce-interval-300ms");
    expect(contains(src, "addPathSafe(gitDir + QStringLiteral(\"/HEAD\"))"),
           "I7/watch-git-HEAD");
    expect(contains(src, "addPathSafe(gitDir + QStringLiteral(\"/index\"))"),
           "I7/watch-git-index");
    expect(contains(src, "addPathSafe(gitDir + QStringLiteral(\"/refs/heads\"))"),
           "I7/watch-git-refs-heads");
    expect(contains(src, "addPathSafe(gitDir + QStringLiteral(\"/refs/remotes\"))"),
           "I7/watch-git-refs-remotes");
    expect(contains(src, "addPathSafe(gitDir + QStringLiteral(\"/logs/HEAD\"))"),
           "I7/watch-git-logs-HEAD");
    expect(contains(src, "QFileSystemWatcher::fileChanged"),
           "I7/connect-fileChanged");
    expect(contains(src, "QFileSystemWatcher::directoryChanged"),
           "I7/connect-directoryChanged");

    // I8 — the fs-event handler re-adds the path. The pattern is
    // the addPathSafe call inside the onFsEvent lambda.
    expect(contains(src, "auto onFsEvent = [watcher, debounce"),
           "I8/onFsEvent-lambda-defined");
    expect(contains(src,
               "!watcher->files().contains(path)"),
           "I8/onFsEvent-checks-watch-state-before-readd");

    // I9 — manual Refresh button bypasses debounce by calling
    // runProbes directly.
    expect(contains(src,
               "setObjectName(QStringLiteral(\"reviewRefreshBtn\"))"),
           "I9/refresh-button-objectName");
    expect(contains(src, "connect(refreshBtn, &QPushButton::clicked"),
           "I9/refresh-button-connected");

    // I10 — live status label.
    expect(contains(src,
               "setObjectName(QStringLiteral(\"reviewLiveStatus\"))"),
           "I10/live-status-label-objectName");
    expect(contains(src, "● live — auto-refresh on git changes"),
           "I10/live-status-finalize-text");
    expect(contains(src, "● refreshing…"),
           "I10/live-status-refreshing-text");

    if (g_failures) {
        std::fprintf(stderr, "\n%d invariant(s) failed\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nall invariants hold\n");
    return 0;
}
