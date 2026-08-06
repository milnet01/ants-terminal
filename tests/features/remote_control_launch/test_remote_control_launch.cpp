// Remote-control `launch` — source-grep regression test. See spec.md.

#include "../../_support/srcgrep.h"

#include <cstdio>
#include <regex>
#include <string>


#include <gtest/gtest.h>
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
#endif
#ifndef SRC_MAIN_CPP
#error "SRC_MAIN_CPP compile definition required"
#endif

static int runMain() {
    const std::string rc  = ants_test::slurpRemoteControl();
    const std::string mwc = ants_test::slurpFile(SRC_MAINWINDOW_CPP);
    const std::string mc  = ants_test::slurpFile(SRC_MAIN_CPP);
    if (rc.empty() || mwc.empty() || mc.empty()) {
        // ANTS-2060 — return failure (not std::exit, which would abort the
        // whole shared gtest bundle); the caller's ASSERT_EQ(0, runMain())
        // then fails just this test.
        std::fprintf(stderr, "cannot open source files\n");
        return 1;
    }

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: dispatch routes "launch".
    std::regex routeLaunch(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"launch"\s*\)[\s\S]{0,200}?cmdLaunch)");
    if (!std::regex_search(rc, routeLaunch)) {
        fail("INV-1: dispatch must route \"launch\" to cmdLaunch");
    }

    // INV-2 + INV-3 + INV-4: cmdLaunch shape. Brace-matched body
    // slurp via the shared srcgrep.h helper (ANTS-1386) so this
    // test stays robust against future body growth — the previous
    // 3500-char fixed window broke during ANTS-1347 when the
    // validatePath block landed.
    const std::string body =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdLaunch");
    if (body.empty()) {
        fail("INV-2a: RemoteControl::cmdLaunch definition missing");
    } else {
        if (body.find("isString()") == std::string::npos) {
            fail("INV-2b: cmdLaunch must validate `command` via isString()");
        }
        if (body.find("missing or empty") == std::string::npos
            || body.find("use new-tab") == std::string::npos) {
            fail("INV-2c: cmdLaunch must emit the documented error message "
                 "with the new-tab suggestion when command is missing/empty");
        }
        // INV-3: auto-append \n when not already present.
        std::regex autoNewline(R"(endsWith\s*\(\s*'\\n'\s*\)[\s\S]{0,80}?\+=\s*'\\n')");
        if (!std::regex_search(body, autoNewline)) {
            fail("INV-3: cmdLaunch must auto-append \\n to command when not "
                 "already present (the convenience contract)");
        }
        // INV-4: delegates to newTabForRemote.
        if (body.find("newTabForRemote") == std::string::npos) {
            fail("INV-4: cmdLaunch must delegate to MainWindow::newTabForRemote "
                 "rather than re-implementing tab creation");
        }
        // INV-4b (added 0.7.52): command must be routed through
        // filterControlChars by default so a same-UID attacker reaching
        // the rc socket can't inject ESC sequences via launch the way
        // they were blocked from doing via send-text. The `raw: true`
        // opt-out is also required for symmetry with send-text.
        if (body.find("filterControlChars") == std::string::npos) {
            fail("INV-4b: cmdLaunch must filter command through "
                 "filterControlChars by default (parity with send-text)");
        }
        if (body.find("\"raw\"") == std::string::npos) {
            fail("INV-4c: cmdLaunch must support a `raw: true` opt-out "
                 "for callers that need raw byte access");
        }
    }

    // INV-5: newTabForRemote uses sendToPty (raw bytes, no auto-\n).
    const std::string ntBody =
        ants_test::slurpFunctionBody(mwc, "MainWindow::newTabForRemote");
    if (ntBody.empty()) {
        fail("INV-5a: MainWindow::newTabForRemote definition missing");
    } else {
        if (ntBody.find("sendToPty(") == std::string::npos) {
            fail("INV-5b: newTabForRemote must use sendToPty (raw bytes) for "
                 "the deferred command write — keeps the documented "
                 "\"caller owns newline\" contract honest");
        }
        if (ntBody.find("writeCommand(") != std::string::npos) {
            fail("INV-5c (neg): newTabForRemote must NOT use writeCommand — "
                 "writeCommand auto-appends \\n, which would silently turn "
                 "every new-tab --remote-command into a launch and break "
                 "the byte-faithful contract");
        }
    }

    // INV-6: Client CLI shares --remote-cwd / --remote-command between new-tab + launch.
    std::regex sharedBranch(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"new-tab"\s*\)\s*\|\|\s*cmd\s*==\s*QLatin1String\s*\(\s*"launch"\s*\))");
    if (!std::regex_search(mc, sharedBranch)) {
        fail("INV-6: main.cpp client must share --remote-cwd / --remote-command "
             "between new-tab and launch via a single `||` branch — keeps the "
             "two commands from drifting on argument forwarding");
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `launch` invariants present\n");
    return 0;
}

TEST(RemoteControlLaunch, Main) {
    ASSERT_EQ(0, runMain());
}
