// PTY write EAGAIN queue — source-grep regression test.
// See spec.md. Fails non-zero if Pty::write reverts to the silent-drop
// behaviour or the write-side notifier / queue / cap is removed.

#include <cstdio>
#include <regex>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#ifndef SRC_PTY_CPP_PATH
#  error "SRC_PTY_CPP_PATH compile definition required"
#endif
#ifndef SRC_PTY_H_PATH
#  error "SRC_PTY_H_PATH compile definition required"
#endif


// Extract the body of the named function. Matches `void Pty::FUNC(...)
// { ... }` with brace-matching. Returns "" if not found.
static std::string extractFnBody(const std::string &src, const char *qualName) {
    std::string pat = std::string("void\\s+") + qualName + R"(\s*\([^)]*\)\s*\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    size_t start = m.position(0) + m.length(0);
    int depth = 1;
    size_t i = start;
    while (i < src.size() && depth > 0) {
        char c = src[i];
        if (c == '{') ++depth;
        else if (c == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    return src.substr(start, i - start - 1);
}

static int runMain() {
    const std::string cpp = ants_test::slurpFile(SRC_PTY_CPP_PATH);
    const std::string hdr = ants_test::slurpFile(SRC_PTY_H_PATH);
    const std::string writeBody = extractFnBody(cpp, "Pty::write");
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    if (writeBody.empty()) {
        fail("precondition: could not locate `void Pty::write(...) { }` "
             "body in src/ptyhandler.cpp.");
    }

    // INV-1 — QSocketNotifier::Write created on m_masterFd in start().
    std::regex writeNotifierCtor(
        R"(new\s+QSocketNotifier\s*\(\s*m_masterFd\s*,\s*QSocketNotifier::Write)");
    if (!std::regex_search(cpp, writeNotifierCtor)) {
        fail("INV-1: no `new QSocketNotifier(m_masterFd, QSocketNotifier::Write)` "
             "found — without a write-side notifier, EAGAIN-queued bytes can "
             "never drain.");
    }

    // INV-2 — m_pendingWrite member declared in header.
    if (hdr.find("m_pendingWrite") == std::string::npos) {
        fail("INV-2: m_pendingWrite member missing from src/ptyhandler.h. "
             "Without the queue there is nowhere to buffer EAGAIN remainders.");
    }

    // INV-3 — m_writeNotifier pointer member declared.
    std::regex writeNotifierMember(
        R"(QSocketNotifier\s*\*\s*m_writeNotifier)");
    if (!std::regex_search(hdr, writeNotifierMember)) {
        fail("INV-3: `QSocketNotifier *m_writeNotifier` missing from "
             "src/ptyhandler.h.");
    }

    // INV-4 — onWriteReady (or equivalent) slot exists and is connected.
    if (hdr.find("onWriteReady") == std::string::npos) {
        fail("INV-4: onWriteReady slot not declared in src/ptyhandler.h.");
    }
    std::regex writeConnect(
        R"(connect\s*\([^;]*&Pty::onWriteReady)");
    if (!std::regex_search(cpp, writeConnect)) {
        fail("INV-4: connect(...&Pty::onWriteReady) binding missing — slot "
             "exists but is never wired to the notifier signal.");
    }

    // INV-5 — Pty::write distinguishes EAGAIN from fatal. The token EAGAIN
    // must appear inside the function body.
    if (!writeBody.empty() && writeBody.find("EAGAIN") == std::string::npos) {
        fail("INV-5: EAGAIN not referenced in Pty::write body. Pre-fix code "
             "lumped EAGAIN with fatal errors and silently dropped bytes; "
             "fix must branch explicitly.");
    }

    // INV-6 — queue capacity bound. Accept any of the common spellings of
    // the 4 MiB cap, plus the named constant.
    bool capped =
        cpp.find("MAX_PENDING_WRITE_BYTES") != std::string::npos ||
        hdr.find("MAX_PENDING_WRITE_BYTES") != std::string::npos ||
        cpp.find("4 * 1024 * 1024") != std::string::npos ||
        cpp.find("4*1024*1024") != std::string::npos ||
        cpp.find("4 << 20") != std::string::npos ||
        cpp.find("4<<20") != std::string::npos;
    if (!capped) {
        fail("INV-6: no queue-size cap (MAX_PENDING_WRITE_BYTES / "
             "4 * 1024 * 1024 / 4 << 20) found. Unbounded m_pendingWrite "
             "growth on a stuck slave OOMs the GUI process.");
    }

    // INV-7 — direct-write path checks queue first (FIFO preservation).
    if (!writeBody.empty()) {
        std::regex queueCheck(
            R"(m_pendingWrite\s*\.\s*(?:isEmpty|empty|size)\s*\()");
        if (!std::regex_search(writeBody, queueCheck)) {
            fail("INV-7: Pty::write does not test m_pendingWrite emptiness "
                 "before issuing a fresh ::write. Fresh bytes can race "
                 "ahead of pending bytes — FIFO ordering broken.");
        }
    }

    // Count non-overlapping occurrences of `needle` in `hay`.
    auto countOf = [](const std::string &hay, const char *needle) {
        size_t n = 0;
        const size_t len = std::char_traits<char>::length(needle);
        for (size_t p = hay.find(needle); p != std::string::npos;
             p = hay.find(needle, p + len)) {
            ++n;
        }
        return n;
    };

    // INV-8 (ANTS-1349 + ANTS-1994(3) + ANTS-4456) — every byte-dropping
    // path in Pty::write emits writeLost so the loss is observable, not
    // silent. There are three drops: the pending-queue-full path, the
    // EAGAIN-oversize-remainder path, and the fatal-write path. All must
    // signal.
    if (!writeBody.empty()) {
        if (countOf(writeBody, "emit writeLost") < 3) {
            fail("INV-8: Pty::write has fewer than 3 'emit writeLost' "
                 "sites. The queue-full drop, the EAGAIN-oversize drop and "
                 "the fatal-error drop must each signal data loss "
                 "(ANTS-1349 / ANTS-1994 / ANTS-4456). The fatal branch "
                 "discards the caller's remainder with only a debug-log "
                 "line, so a torn-down master loses keystrokes silently.");
        }
    }

    // INV-9 / INV-10 (ANTS-4456) — the drain slot must tell a kernel
    // buffer that is full again apart from a write that can never
    // succeed, and must clear the queue on the latter. The notifier is
    // disarmed only when m_pendingWrite is empty, and a dead master FD
    // stays write-ready, so leaving bytes queued after a fatal error
    // re-arms this slot forever and spins the event loop at 100% CPU.
    const std::string drainBody = extractFnBody(cpp, "Pty::onWriteReady");
    if (drainBody.empty()) {
        fail("precondition: could not locate `void Pty::onWriteReady(...) "
             "{ }` body in src/ptyhandler.cpp.");
    } else {
        // Match a comparison, not the bare token: the pre-fix body
        // carried the word EAGAIN in a comment on the very `break` that
        // failed to branch, so a token search passes against the defect.
        std::regex eagainBranch(
            R"(errno\s*==\s*(?:EAGAIN|EWOULDBLOCK))");
        if (!std::regex_search(drainBody, eagainBranch)) {
            fail("INV-9: Pty::onWriteReady never compares errno against "
                 "EAGAIN/EWOULDBLOCK. Pre-fix the drain loop broke out "
                 "identically for kernel back-pressure and for a fatal "
                 "error, so the fatal case was never actually handled.");
        }
        std::regex clearQueue(R"(m_pendingWrite\s*\.\s*clear\s*\()");
        if (!std::regex_search(drainBody, clearQueue)) {
            fail("INV-10: Pty::onWriteReady never calls "
                 "m_pendingWrite.clear(). A fatal drain error leaves the "
                 "queue populated, the write notifier stays armed, and the "
                 "event loop spins at 100% CPU for the life of the "
                 "process with no error surfaced.");
        }
        if (drainBody.find("emit writeLost") == std::string::npos) {
            fail("INV-10: Pty::onWriteReady drops queued bytes on a fatal "
                 "error without emitting writeLost. The loss must be "
                 "observable, as it is on Pty::write's three drop paths.");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/pty_write_eagain_queue/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: PTY write queues on EAGAIN and drains via "
                "QSocketNotifier::Write\n");
    return 0;
}

TEST(PtyWriteEagainQueue, Main) {
    ASSERT_EQ(0, runMain());
}
