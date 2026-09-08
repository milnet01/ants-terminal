// Session log + session recording are owner-only — source-grep test.
// See spec.md. Both files are auto-created in the app's own data dir and
// hold the terminal's full byte stream; neither asked for 0600, and both
// containing directories were created at umask.
//
// Source-grep rather than a runtime stat: these paths need a constructed
// TerminalWidget (PTY + grid) and a MainWindow, which the unit harness
// has neither of. Same reason paste_dialog_custom and
// scratchpad_submit_ordering grep this file.

#include <cstdio>
#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_TERMINALWIDGET_PATH
#  error "SRC_TERMINALWIDGET_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_PATH
#  error "SRC_MAINWINDOW_PATH compile definition required"
#endif

namespace {

// Extract the brace-matched body of a member function, matching on the
// qualified name so a declaration in a header-like context cannot win.
std::string memberBody(const std::string &src, const char *qualName) {
    const std::string pat =
        std::string(R"((?:void|bool|int)\s+)") + qualName + R"(\s*\([^)]*\)[^;{]*\{)";
    std::regex re(pat);
    std::smatch m;
    if (!std::regex_search(src, m, re)) return {};
    size_t i = static_cast<size_t>(m.position(0)) + m.length(0);
    int depth = 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return {};
    const size_t start = static_cast<size_t>(m.position(0)) + m.length(0);
    return src.substr(start, i - start - 1);
}

// The Record Session menu handler in mainwindow.cpp: from the action's
// construction to the startRecording call it drives. Grepping the whole
// file would let an ensurePrivateDir elsewhere satisfy INV-5.
std::string recordHandler(const std::string &src) {
    const size_t anchor = src.find("\"&Record Session\"");
    if (anchor == std::string::npos) return {};
    const size_t end = src.find("startRecording", anchor);
    if (end == std::string::npos) return {};
    return src.substr(anchor, end - anchor);
}

bool calls(const std::string &body, const char *fn) {
    const std::regex re(std::string(fn) + R"(\s*\()");
    return std::regex_search(body, re);
}

// `ensurePrivateDir(...)` whose result is tested rather than discarded:
// negated, returned, or consumed by an if/while condition.
bool guardsPrivateDir(const std::string &body) {
    const std::regex re(
        R"((?:if|while)\s*\(\s*!?\s*ensurePrivateDir|!\s*ensurePrivateDir|return\s+ensurePrivateDir)");
    return std::regex_search(body, re);
}

int runMain() {
    const std::string tw = ants_test::slurpFile(SRC_TERMINALWIDGET_PATH);
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_PATH);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string logBody = memberBody(tw, "TerminalWidget::setSessionLogging");
    const std::string recBody = memberBody(tw, "TerminalWidget::startRecording");
    const std::string handler = recordHandler(mw);

    if (logBody.empty())
        fail("precondition: TerminalWidget::setSessionLogging body not found.");
    if (recBody.empty())
        fail("precondition: TerminalWidget::startRecording body not found.");
    if (handler.empty())
        fail("precondition: the \"&Record Session\" handler up to its "
             "startRecording call was not found in src/mainwindow.cpp.");

    if (!logBody.empty()) {
        // INV-1 — the log directory is born 0700, not created at umask.
        if (!calls(logBody, "ensurePrivateDir")) {
            fail("INV-1: setSessionLogging does not create its log "
                 "directory through ensurePrivateDir. QDir::mkpath uses "
                 "the process umask (0755 on a desktop), so another local "
                 "user can traverse into the directory holding the "
                 "terminal's full output stream.");
        }
        if (std::regex_search(logBody, std::regex(R"(QDir\s*\(\s*\)\s*\.\s*mkpath)"))) {
            fail("INV-1: setSessionLogging still calls QDir().mkpath. The "
                 "umask-derived directory is the hole ensurePrivateDir "
                 "closes; both together leave the window open.");
        }
        // INV-2 — a directory that cannot be secured stops the capture.
        if (!guardsPrivateDir(logBody)) {
            fail("INV-2: setSessionLogging discards the ensurePrivateDir "
                 "result. secureio.h requires a caller to surface false; "
                 "logging into a directory that could not be secured is "
                 "the outcome this feature prevents.");
        }
        // INV-3 — the file itself is 0600.
        if (!calls(logBody, "setOwnerOnlyPerms")) {
            fail("INV-3: setSessionLogging never calls setOwnerOnlyPerms. "
                 "QFile::open creates at the process umask, so the session "
                 "log lands 0644 and is world-readable.");
        }
    }

    if (!recBody.empty()) {
        // INV-4 — the recording file is 0600, checked on the widget so any
        // future caller inherits it.
        if (!calls(recBody, "setOwnerOnlyPerms")) {
            fail("INV-4: startRecording never calls setOwnerOnlyPerms. The "
                 "asciicast holds the same byte stream as the session log "
                 "and lands 0644.");
        }
    }

    if (!handler.empty()) {
        // INV-5 — the recordings directory is born 0700.
        if (!calls(handler, "ensurePrivateDir")) {
            fail("INV-5: the Record Session handler does not create its "
                 "recordings directory through ensurePrivateDir.");
        }
        if (std::regex_search(handler, std::regex(R"(QDir\s*\(\s*\)\s*\.\s*mkpath)"))) {
            fail("INV-5: the Record Session handler still calls "
                 "QDir().mkpath for the recordings directory.");
        }
        if (!guardsPrivateDir(handler)) {
            fail("INV-5: the Record Session handler discards the "
                 "ensurePrivateDir result, so a directory that could not "
                 "be secured is recorded into anyway.");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see "
            "tests/features/session_capture_perms/spec.md for context\n",
            failures);
        return 1;
    }
    std::printf("OK: session log and session recording are owner-only\n");
    return 0;
}

}  // namespace

TEST(SessionCapturePerms, Main) {
    ASSERT_EQ(0, runMain());
}
