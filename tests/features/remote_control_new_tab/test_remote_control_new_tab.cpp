// Remote-control `new-tab` — source-grep regression test. See spec.md.

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
    const std::string mwh = slurp(SRC_MAINWINDOW_H);
    const std::string mwc = slurp(SRC_MAINWINDOW_CPP);
    const std::string mc  = slurp(SRC_MAIN_CPP);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: dispatch routes "new-tab" to cmdNewTab.
    std::regex routeNewTab(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"new-tab"\s*\)[\s\S]{0,200}?cmdNewTab)");
    if (!std::regex_search(rc, routeNewTab)) {
        fail("INV-1: RemoteControl::dispatch must route \"new-tab\" to cmdNewTab");
    }

    // INV-2 + INV-3: cmdNewTab reads cwd + command, delegates to
    // newTabForRemote, returns index field.
    size_t ntPos = rc.find("RemoteControl::cmdNewTab");
    if (ntPos == std::string::npos) {
        fail("INV-2a: RemoteControl::cmdNewTab definition missing");
    } else {
        std::string body = rc.substr(ntPos, 1500);
        if (body.find("\"cwd\"")     == std::string::npos ||
            body.find("\"command\"") == std::string::npos) {
            fail("INV-2b: cmdNewTab must read both \"cwd\" and \"command\" fields");
        }
        if (body.find("newTabForRemote") == std::string::npos) {
            fail("INV-3a: cmdNewTab must delegate to MainWindow::newTabForRemote");
        }
        if (body.find("\"index\"") == std::string::npos) {
            fail("INV-3b: cmdNewTab must return the tab index under the \"index\" key");
        }
    }

    // INV-4: newTabForRemote signature — non-const, returns int.
    std::regex ntSig(
        R"(int\s+newTabForRemote\s*\(\s*const\s+QString\s*&\s*\w+\s*,\s*const\s+QString\s*&\s*\w+\s*\)\s*;)");
    if (!std::regex_search(mwh, ntSig)) {
        fail("INV-4: MainWindow::newTabForRemote(const QString&, const QString&) "
             "declaration missing from mainwindow.h — must be non-const, "
             "return int (the new tab's index)");
    }

    // INV-5: 200 ms singleShot for the command write, and INV-6: QPointer guard.
    size_t ntImpl = mwc.find("MainWindow::newTabForRemote");
    if (ntImpl == std::string::npos) {
        fail("INV-5a: MainWindow::newTabForRemote definition missing from mainwindow.cpp");
    } else {
        std::string body = mwc.substr(ntImpl, 2000);
        std::regex singleShot200(R"(QTimer::singleShot\s*\(\s*200\s*,)");
        if (!std::regex_search(body, singleShot200)) {
            fail("INV-5b: newTabForRemote must use QTimer::singleShot(200, ...) "
                 "for the command write — matches onSshConnect timing");
        }
        if (body.find("QPointer<TerminalWidget>") == std::string::npos) {
            fail("INV-6: newTabForRemote must use QPointer<TerminalWidget> to guard "
                 "the deferred lambda — a tab close between enqueue and fire would "
                 "UB on a freed widget");
        }
    }

    // INV-7: client CLI adds --remote-cwd / --remote-command options.
    if (mc.find("\"remote-cwd\"")     == std::string::npos ||
        mc.find("\"remote-command\"") == std::string::npos) {
        fail("INV-7a: main.cpp must register --remote-cwd and --remote-command "
             "QCommandLineOptions for new-tab / launch");
    }
    size_t mainNtPos = mc.find("\"new-tab\"");
    if (mainNtPos == std::string::npos) {
        fail("INV-7b: main.cpp must branch on the \"new-tab\" command to forward "
             "the cwd + command args");
    } else {
        std::string block = mc.substr(mainNtPos, 1000);
        if (block.find("args[\"cwd\"]")     == std::string::npos ||
            block.find("args[\"command\"]") == std::string::npos) {
            fail("INV-7c: new-tab client branch must forward cwd + command args "
                 "into the JSON envelope");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `new-tab` invariants present\n");
    return 0;
}
