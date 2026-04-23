// Remote-control `send-text` — source-grep regression test.
// See spec.md.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H
#error "SRC_MAINWINDOW_H compile definition required"
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
    const std::string rc  = slurp(SRC_RC_CPP);
    const std::string mwh = slurp(SRC_MAINWINDOW_H);
    const std::string mc  = slurp(SRC_MAIN_CPP);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: dispatch routes "send-text" to cmdSendText.
    std::regex routeSendText(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"send-text"\s*\)[\s\S]{0,200}?cmdSendText)");
    if (!std::regex_search(rc, routeSendText)) {
        fail("INV-1: RemoteControl::dispatch must route \"send-text\" to cmdSendText — "
             "drift would fall through to unknown-command and break automation");
    }

    // INV-2: cmdSendText validates text via isString.
    size_t cstPos = rc.find("RemoteControl::cmdSendText");
    if (cstPos == std::string::npos) {
        fail("INV-2a: RemoteControl::cmdSendText definition missing");
    } else {
        // Window widened from 2000 → 4000 in 0.7.12 when the opt-in
        // filter (remote_control_opt_in spec) expanded cmdSendText's
        // body with the raw-bypass + filterControlChars delegation.
        std::string body = rc.substr(cstPos, 4000);
        if (body.find("isString()") == std::string::npos) {
            fail("INV-2b: cmdSendText must validate the `text` field via isString() — "
                 "matches Kitty's \"text is required and must be a string\" contract");
        }
        // INV-3: tab distinguished via isDouble (not `toInt() == 0`).
        if (body.find("isDouble()") == std::string::npos) {
            fail("INV-3: cmdSendText must use isDouble() to distinguish \"tab omitted\" "
                 "from \"tab 0\" — toInt()-based check silently targets tab 0");
        }
        // INV-4: calls sendToPty on the target.
        if (body.find("sendToPty(") == std::string::npos) {
            fail("INV-4: cmdSendText must call sendToPty on the target terminal");
        }
        // INV-7 (negative): must not auto-append a newline to `text`.
        // Auto-newline would surprise binary-stream callers. Detection
        // heuristic: scan for "text +" or "text += \"\\n\"" which would
        // be the typical spelling of such a bug.
        if (std::regex_search(body, std::regex(R"(text\s*(?:\+|\+=))"))) {
            fail("INV-7 (neg): cmdSendText must NOT auto-append a newline to `text` — "
                 "diverges from Kitty semantics and breaks binary-stream callers");
        }
    }

    // INV-5: MainWindow::terminalAtTab public + signature.
    std::regex atTabDecl(
        R"(TerminalWidget\s*\*\s*terminalAtTab\s*\(\s*int\s+\w+\s*\)\s*const\s*;)");
    if (!std::regex_search(mwh, atTabDecl)) {
        fail("INV-5: MainWindow::terminalAtTab(int) const declaration missing "
             "from mainwindow.h (must be public)");
    }

    // INV-6: main.cpp reads stdin when --remote-text is absent for send-text.
    size_t mainSendPos = mc.find("\"send-text\"");
    if (mainSendPos == std::string::npos) {
        fail("INV-6a: main.cpp must branch on the \"send-text\" command to handle "
             "the stdin-read ergonomic");
    } else {
        std::string sendBlock = mc.substr(mainSendPos, 1000);
        if (sendBlock.find("stdin") == std::string::npos) {
            fail("INV-6b: the send-text client branch must read stdin when "
                 "--remote-text is absent — shell-pipe ergonomics");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `send-text` invariants present\n");
    return 0;
}
