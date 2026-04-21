// Remote-control `launch` — source-grep regression test. See spec.md.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
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
    const std::string mwc = slurp(SRC_MAINWINDOW_CPP);
    const std::string mc  = slurp(SRC_MAIN_CPP);

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

    // INV-2 + INV-3 + INV-4: cmdLaunch shape.
    size_t lPos = rc.find("RemoteControl::cmdLaunch");
    if (lPos == std::string::npos) {
        fail("INV-2a: RemoteControl::cmdLaunch definition missing");
    } else {
        std::string body = rc.substr(lPos, 1500);
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
    }

    // INV-5: newTabForRemote uses sendToPty (raw bytes, no auto-\n).
    size_t ntPos = mwc.find("MainWindow::newTabForRemote");
    if (ntPos == std::string::npos) {
        fail("INV-5a: MainWindow::newTabForRemote definition missing");
    } else {
        std::string body = mwc.substr(ntPos, 2500);
        if (body.find("sendToPty(") == std::string::npos) {
            fail("INV-5b: newTabForRemote must use sendToPty (raw bytes) for "
                 "the deferred command write — keeps the documented "
                 "\"caller owns newline\" contract honest");
        }
        if (body.find("writeCommand(") != std::string::npos) {
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
