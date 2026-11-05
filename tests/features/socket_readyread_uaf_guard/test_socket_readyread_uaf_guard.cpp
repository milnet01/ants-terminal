// Feature-conformance test for spec.md — ANTS-2102.
//
// Source-grep regression lock for the nested-loop socket
// use-after-free crash class (ANTS-2101/2026/2103/2104). A dispatching
// QLocalSocket::readyRead handler that pumps a nested QEventLoop on the
// main thread can have the live socket freed mid-dispatch (peer
// disconnect -> deleteLater processed by the nested loop) and then write
// to freed memory. The fixes are two guards on the buffering handlers
// (idleTimer->stop() + QPointer<QLocalSocket>) and worker-thread
// isolation for the two heavy verbs (audit_run, indie_review_dispatch).
//
// A live-socket runtime reproduction is impractical (needs a real
// QLocalServer + external audit tools + a deterministic mid-dispatch
// disconnect), so — like qpointer_destroyed_safe (ANTS-1320/1324) — this
// is a static grep lock: it fails the moment any guard is removed.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <regex>
#include <string>
#include <gtest/gtest.h>

#ifndef ANTS_CLAUDEINTEGRATION_SOURCES
#  error "ANTS_CLAUDEINTEGRATION_SOURCES compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#  error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef ANTS_MAINWINDOW_SOURCES
#  error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

// True iff `needle` appears in the `span`-char window of `hay` starting
// at the first occurrence of `marker`. Used to assert that a guard token
// follows the handler it must protect, without parsing the lambda body.
bool windowHas(const std::string &hay, const std::string &marker,
               size_t span, const std::string &needle) {
    const size_t at = hay.find(marker);
    if (at == std::string::npos) return false;
    const size_t found = hay.find(needle, at);
    return found != std::string::npos && found < at + span;
}

}  // namespace

TEST(SocketReadyreadUafGuard, Main) {
    expect_reset();

    const std::string claude =
        ants_test::slurpClaudeIntegration();
    const std::string remote =
        ants_test::slurpRemoteControl();
    const std::string mainwin =
        ants_test::slurpMainWindow();

    expect(!claude.empty(), "load/claudeintegration.cpp");
    expect(!remote.empty(), "load/remotecontrol.cpp");
    expect(!mainwin.empty(), "load/mainwindow.cpp");

    // INV-1 — the MCP readyRead handler (the one capturing idleTimer)
    // stops the slow-loris timer and QPointer-guards the socket before
    // dispatch. The hook handler (`[socket, buf, overCap]`) is short-op-only and is
    // intentionally not required to carry these. (ANTS-2101)
    // ANTS-4932 — the reply goes through the connection's McpReplyChannel, a
    // child of the socket, so a QPointer to the channel goes null exactly when
    // the socket is freed. handleMcpLine() takes that guard.
    {
        const std::string mcpMarker =
            "&QLocalSocket::readyRead, this, [this, socket, idleTimer, channel, buf]";
        expect(claude.find(mcpMarker) != std::string::npos,
               "INV-1/mcp-readyread-handler-present");
        expect(windowHas(claude, mcpMarker, 3000, "idleTimer->stop()"),
               "INV-1/mcp-stops-idle-timer-before-dispatch");
        expect(windowHas(claude, mcpMarker, 3000, "QPointer<McpReplyChannel>"),
               "INV-1/mcp-qpointer-guards-socket");
    }

    // INV-2 — the remote-control readyRead handler does the same pair
    // before dispatch(). remotecontrol.cpp has exactly one readyRead
    // handler, so anchor on the signal alone. (ANTS-2026)
    {
        const std::string rcMarker = "&QLocalSocket::readyRead";
        expect(remote.find(rcMarker) != std::string::npos,
               "INV-2/remote-readyread-handler-present");
        expect(windowHas(remote, rcMarker, 3000, "idleTimer->stop()"),
               "INV-2/remote-stops-idle-timer-before-dispatch");
        expect(windowHas(remote, rcMarker, 3000, "QPointer<QLocalSocket>"),
               "INV-2/remote-qpointer-guards-socket");
    }

    // INV-3 — audit_run runs on a worker thread so its per-tool
    // QProcess QEventLoop never pumps on the dispatching (main) thread.
    // (ANTS-2103) Assert a QThread::create lambda calls runAudit.
    {
        std::regex auditWorker(
            R"(QThread::create\([\s\S]{0,160}AuditRunner::runAudit)");
        expect(std::regex_search(mainwin, auditWorker),
               "INV-3/audit_run-dispatched-on-worker-thread");
    }

    // INV-4 — indie_review_dispatch runs on a worker thread for the
    // identical QNAM + QEventLoop hazard. (ANTS-2104)
    {
        std::regex indieWorker(
            R"(QThread::create\([\s\S]{0,220}cmdIndieReviewDispatch)");
        expect(std::regex_search(mainwin, indieWorker),
               "INV-4/indie_review_dispatch-dispatched-on-worker-thread");
    }

    EXPECT_EQ(0, expect_failures());
}

// ANTS-6397 — the hook path parses its buffer on disconnect, so an over-cap
// read must empty it first: otherwise the ANTS-1659 256 KiB cap fires and the
// disconnected handler parses the whole oversized payload anyway.
TEST(SocketReadyreadUafGuard, Ants6397HookOverCapEmptiesTheBuffer) {
    const std::string claude = ants_test::slurpClaudeIntegration();
    const std::string marker = "&QLocalSocket::readyRead, this, [socket, buf, overCap]()";
    const size_t at = claude.find(marker);
    ASSERT_NE(at, std::string::npos) << "hook readyRead handler not found";
    const size_t cap = claude.find("buf->size() > 256 * 1024", at);
    ASSERT_NE(cap, std::string::npos);
    const size_t clear = claude.find("buf->clear()", cap);
    const size_t disc = claude.find("disconnectFromServer()", cap);
    ASSERT_NE(disc, std::string::npos);
    EXPECT_TRUE(clear != std::string::npos && clear < disc)
        << "the over-cap branch disconnects with the oversized bytes still buffered";
}
