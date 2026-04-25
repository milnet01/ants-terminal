// PTY write EAGAIN queue — source-grep regression test.
// See spec.md. Fails non-zero if Pty::write reverts to the silent-drop
// behaviour or the write-side notifier / queue / cap is removed.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_PTY_CPP_PATH
#  error "SRC_PTY_CPP_PATH compile definition required"
#endif
#ifndef SRC_PTY_H_PATH
#  error "SRC_PTY_H_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

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

int main() {
    const std::string cpp = slurp(SRC_PTY_CPP_PATH);
    const std::string hdr = slurp(SRC_PTY_H_PATH);
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
