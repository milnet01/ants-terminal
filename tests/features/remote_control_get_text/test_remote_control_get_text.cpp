// Remote-control `get-text` — source-grep regression test. See spec.md.

#include "../../_support/srcgrep.h"

#include <cstdio>
#include <regex>
#include <string>


#include <gtest/gtest.h>
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAIN_CPP
#error "SRC_MAIN_CPP compile definition required"
#endif

static int runMain() {
    const std::string rc = ants_test::slurpFile(SRC_RC_CPP);
    const std::string mc = ants_test::slurpFile(SRC_MAIN_CPP);
    if (rc.empty() || mc.empty()) {
        std::fprintf(stderr, "cannot open source files\n");
        std::exit(2);
    }

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: dispatch routes "get-text" to cmdGetText.
    std::regex routeGt(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"get-text"\s*\)[\s\S]{0,200}?cmdGetText)");
    if (!std::regex_search(rc, routeGt)) {
        fail("INV-1: dispatch must route \"get-text\" to cmdGetText");
    }

    // INV-2 + INV-3 + INV-4 + INV-5: shape of cmdGetText. Brace-matched
    // body slurp via the shared srcgrep.h helper so this test stays
    // robust against future body growth (the previous fixed-size
    // 2500-char window broke once during ANTS-1348 — INV-5's "bytes"
    // field check fell outside the window after the new max_bytes
    // block landed).
    const std::string body =
        ants_test::slurpFunctionBody(rc, "RemoteControl::cmdGetText");
    if (body.empty()) {
        fail("INV-2a: RemoteControl::cmdGetText definition missing "
             "(or unbalanced braces)");
    } else {
        if (body.find("isDouble()") == std::string::npos) {
            fail("INV-2b: cmdGetText must use isDouble() for tab + lines "
                 "(consistent with send-text / set-title / select-window)");
        }
        if (body.find("recentOutput(") == std::string::npos) {
            fail("INV-3: cmdGetText must reuse TerminalWidget::recentOutput — "
                 "the AI dialog already uses it; both consumers should share the "
                 "same accessor so format drift between them is impossible");
        }
        if (body.find("10000") == std::string::npos) {
            fail("INV-4: cmdGetText must cap `lines` at 10000 so a script that "
                 "passes a huge value against a huge scrollback doesn't return "
                 "a 100 MB JSON envelope");
        }
        for (const char *field : {"\"text\"", "\"lines\"", "\"bytes\""}) {
            if (body.find(field) == std::string::npos) {
                std::string msg = "INV-5: cmdGetText response must carry field ";
                msg += field;
                std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
                ++failures;
            }
        }
    }

    // INV-6: Client CLI rejects malformed --remote-lines.
    size_t mainGtPos = mc.find("\"get-text\"");
    if (mainGtPos == std::string::npos) {
        fail("INV-6a: main.cpp must branch on the \"get-text\" command to "
             "forward the lines arg");
    } else {
        std::string block = mc.substr(mainGtPos, 1000);
        if (block.find("toInt(&ok)") == std::string::npos) {
            fail("INV-6b: get-text client branch must validate --remote-lines "
                 "via toInt(&ok) and reject malformed input — silently sending "
                 "garbage would surface as an opaque server error");
        }
        if (block.find("args[\"lines\"]") == std::string::npos) {
            fail("INV-6c: get-text client branch must forward the parsed lines "
                 "value as `lines` in the JSON envelope");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `get-text` invariants present\n");
    return 0;
}

TEST(RemoteControlGetText, Main) {
    ASSERT_EQ(0, runMain());
}
