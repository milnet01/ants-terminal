// Remote-control `select-window` — source-grep regression test.
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
#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
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

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: dispatch routes "select-window" to cmdSelectWindow.
    std::regex routeSel(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"select-window"\s*\)[\s\S]{0,200}?cmdSelectWindow)");
    if (!std::regex_search(rc, routeSel)) {
        fail("INV-1: dispatch must route \"select-window\" to cmdSelectWindow");
    }

    // INV-2 + INV-3: cmdSelectWindow validates tab via isDouble and rejects missing.
    size_t swPos = rc.find("RemoteControl::cmdSelectWindow");
    if (swPos == std::string::npos) {
        fail("INV-2a: RemoteControl::cmdSelectWindow definition missing");
    } else {
        std::string body = rc.substr(swPos, 1500);
        if (body.find("isDouble()") == std::string::npos) {
            fail("INV-3: cmdSelectWindow must validate `tab` via isDouble() — "
                 "toInt()-based check silently accepts string-typed input as tab 0");
        }
        if (body.find("missing or non-integer") == std::string::npos) {
            fail("INV-2b: cmdSelectWindow must emit a 'missing or non-integer \"tab\"' "
                 "error when tab is absent — no implicit default-to-active, caller "
                 "always spells out which tab they mean");
        }
        if (body.find("selectTabForRemote") == std::string::npos) {
            fail("INV-2c: cmdSelectWindow must delegate to MainWindow::selectTabForRemote");
        }
    }

    // INV-4: selectTabForRemote(int) public, non-const, returns bool.
    std::regex sig(R"(bool\s+selectTabForRemote\s*\(\s*int\s+\w+\s*\)\s*;)");
    if (!std::regex_search(mwh, sig)) {
        fail("INV-4: MainWindow::selectTabForRemote(int) declaration missing "
             "from mainwindow.h — must return bool (false on out-of-range), non-const");
    }

    // INV-5 + INV-6: implementation calls setFocus + bounds-checks via count().
    size_t impl = mwc.find("MainWindow::selectTabForRemote");
    if (impl == std::string::npos) {
        fail("INV-5a: MainWindow::selectTabForRemote definition missing");
    } else {
        std::string body = mwc.substr(impl, 800);
        if (body.find("setFocus(") == std::string::npos) {
            fail("INV-5b: selectTabForRemote must call setFocus on the new active "
                 "terminal — without it, follow-up send-text calls hit a stale "
                 "focus owner (menubar, dialog button, search bar)");
        }
        if (body.find("count()") == std::string::npos) {
            fail("INV-6: selectTabForRemote must bounds-check against m_tabWidget->count() "
                 "and return false on out-of-range — dispatch layer renders that as "
                 "the no-tab-at-index error envelope");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `select-window` invariants present\n");
    return 0;
}
