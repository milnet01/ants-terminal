// Remote-control `get-text` — source-grep regression test. See spec.md.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAIN_CPP
#error "SRC_MAIN_CPP compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string rc = slurp(SRC_RC_CPP);
    const std::string mc = slurp(SRC_MAIN_CPP);

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

    // INV-2 + INV-3 + INV-4 + INV-5: shape of cmdGetText.
    size_t gtPos = rc.find("RemoteControl::cmdGetText");
    if (gtPos == std::string::npos) {
        fail("INV-2a: RemoteControl::cmdGetText definition missing");
    } else {
        std::string body = rc.substr(gtPos, 2500);
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
