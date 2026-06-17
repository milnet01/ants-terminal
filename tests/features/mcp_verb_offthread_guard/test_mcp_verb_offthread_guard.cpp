// Feature-conformance test for spec.md — ANTS-2131.
//
// Source-grep lock for the structural end-state of the nested-loop
// socket use-after-free thread (ANTS-2102 part-2 / ANTS-2103/2104): every
// MCP verb that blocks the main thread now dispatches off it. The two
// QEventLoop verbs (audit_run, indie_review_dispatch) are locked by
// ANTS-2102; this locks the QProcess::waitForFinished verbs
// (verify_changes, debt_sweep_*), which register through the
// rcDelegateWorker factory instead of the synchronous rcDelegate.
//
// Like ANTS-2102, a live-socket runtime reproduction is impractical, so
// this is a static grep lock: it fails the moment a guarded verb is
// reverted to the synchronous delegate.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <string>
#include <vector>
#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// True iff every needle appears within `span` chars of the first
// occurrence of `marker` in `hay`. Used to assert a verb registration is
// followed by the worker-delegate call without parsing the call args.
bool windowHasAll(const std::string &hay, const std::string &marker,
                  size_t span, const std::vector<std::string> &needles) {
    const size_t at = hay.find(marker);
    if (at == std::string::npos) return false;
    for (const auto &needle : needles) {
        const size_t found = hay.find(needle, at);
        if (found == std::string::npos || found >= at + span) return false;
    }
    return true;
}

}  // namespace

TEST(McpVerbOffthreadGuard, Main) {
    expect_reset();

    const std::string mw =
        ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(!mw.empty(), "load/mainwindow.cpp");

    // INV-1 — the rcDelegateWorker factory runs the delegated cmd*() call
    // on a QThread::create worker and joins it (worker->wait(), a join,
    // never an event pump). Anchor on the factory binding, then assert the
    // worker-thread machinery follows within the factory body.
    {
        const std::string factoryMarker = "auto rcDelegateWorker =";
        expect(mw.find(factoryMarker) != std::string::npos,
               "INV-1/rcDelegateWorker-factory-present");
        expect(windowHasAll(mw, factoryMarker, 900,
                            {"QThread::create", "->*fn)(args)",
                             "worker->wait()"}),
               "INV-1/rcDelegateWorker-runs-on-joined-worker");
    }

    // INV-2 — verify_changes registers through rcDelegateWorker, not the
    // synchronous rcDelegate. The exact `rcDelegateWorker(&...cmdVerify...)`
    // substring discriminates against a revert to `rcDelegate(&...)`
    // (rcDelegateWorker contains rcDelegate, so match the full token).
    expect(mw.find("rcDelegateWorker(&RemoteControl::cmdVerifyChanges)")
               != std::string::npos,
           "INV-2/verify_changes-off-main-thread");

    // INV-3 — all four debt_sweep_* verbs register through rcDelegateWorker.
    for (const char *fn : {"cmdDebtSweepScan", "cmdDebtSweepApplyFix",
                           "cmdDebtSweepDefer", "cmdDebtSweepTriagePrompt"}) {
        const std::string needle =
            std::string("rcDelegateWorker(&RemoteControl::") + fn + ")";
        expect(mw.find(needle) != std::string::npos,
               "INV-3/debt_sweep-off-main-thread", fn);
    }

    EXPECT_EQ(0, expect_failures());
}
