// Feature-conformance test for spec.md —
//
// Source-grep test. The Review Changes dialog must run the two
// new branch-aware probes alongside the original three, render
// both new sections, and include them in the Copy Diff payload.

#include "../../_support/expect.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_DIFFVIEWER_CPP_PATH
#  error "SRC_DIFFVIEWER_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {




bool contains(const std::string &h, const std::string &n) {
    return h.find(n) != std::string::npos;
}

}  // namespace

TEST(ReviewChangesBranches, Main) {
    expect_reset();
    const std::string src = ants_test::slurpFile(SRC_DIFFVIEWER_CPP_PATH);

    // I1 — ProbeState declares the new fields.
    expect(contains(src, "QString branches;"),
           "I1/probestate-branches-field");
    expect(contains(src, "QString crossUnpushed;"),
           "I1/probestate-crossUnpushed-field");
    // ANTS-1601 — assert the field is initialised, not the exact count.
    // `pending` is the number of baseline async probes; pinning it to the
    // literal 5 makes the test break the moment a 6th probe is added even
    // though the code is correct. The per-probe wirings are asserted by
    // name below (for-each-ref / branches / crossUnpushed), so the count
    // literal is the brittle, redundant part.
    expect(contains(src, "int pending = "),
           "I1/probestate-pending-counter-initialised",
           "expected `int pending = <baseline probe count>;`");

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
    // *inside* the lambda body, not before it. ANTS-1145 (0.7.73):
    // capture changed from `[this, ...]` to `[parent, ...]` when the
    // function moved from MainWindow::showDiffViewer to a free
    // diffviewer::show — `this` doesn't exist in the free form.
    expect(contains(src, "auto runProbes = [parent, cwd, dlgGuard"),
           "I6/runProbes-lambda-defined");
    expect(contains(src, "auto state = std::make_shared<ProbeState>();"),
           "I6/runProbes-constructs-fresh-state-per-call");

    // I7 (ANTS-3509) — live refresh runs on the raw-inotify DirTreeWatcher.
    // QFileSystemWatcher was replaced because its directory watch does not fire
    // on a content EDIT of a child file (only add/remove/rename), so an
    // editor/agent edit left the diff stale until a commit or manual Refresh.
    expect(contains(src, "auto *watcher = new DirTreeWatcher(dialog);"),
           "I7/dirtreewatcher-constructed-on-dialog");
    // Watch set seeded from a gitignore-aware ls-files (build/ etc. excluded by
    // construction — inotify has no native exclude, so whitelisting is the
    // mechanism), mapped to directories via the pure helper.
    expect(contains(src, "--exclude-standard"),
           "I7/seed-is-gitignore-aware");
    expect(contains(src, "DirTreeWatcher::directoriesContaining("),
           "I7/seed-maps-files-to-dirs");
    // Git dir resolved (worktree / sub-dir cwd safe), not assumed at cwd/.git.
    expect(contains(src, "--absolute-git-dir") &&
               contains(src, "--show-toplevel"),
           "I7/git-dir-resolved-via-rev-parse");
    // .git metadata dirs watched for staging/commit/branch/fetch.
    expect(contains(src, "/refs/heads") && contains(src, "/refs/remotes") &&
               contains(src, "/logs"),
           "I7/watch-git-metadata-dirs");
    expect(contains(src, "DirTreeWatcher::changed"),
           "I7/connect-changed");

    // I8 (ANTS-3509) — re-probe + re-seed on any change; and probes run with
    // GIT_OPTIONAL_LOCKS=0 so a read-only status can't rewrite .git/index and
    // self-trigger a loop against the now-reliable inotify watch.
    expect(contains(src, "reseed(); runProbes();"),
           "I8/change-reseeds-and-reprobes");
    expect(contains(src, "GIT_OPTIONAL_LOCKS"),
           "I8/probes-disable-optional-index-locks");

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

    ASSERT_EQ(0, expect_finish());
    std::fprintf(stderr, "\nall invariants hold\n");
    return;
}

