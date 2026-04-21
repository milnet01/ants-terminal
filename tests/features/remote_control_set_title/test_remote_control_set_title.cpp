// Remote-control `set-title` — source-grep regression test. See spec.md.

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

    // INV-1: dispatch routes "set-title".
    std::regex routeSt(
        R"(cmd\s*==\s*QLatin1String\s*\(\s*"set-title"\s*\)[\s\S]{0,200}?cmdSetTitle)");
    if (!std::regex_search(rc, routeSt)) {
        fail("INV-1: dispatch must route \"set-title\" to cmdSetTitle");
    }

    // INV-2 + INV-3: validation + tab fallback.
    size_t stPos = rc.find("RemoteControl::cmdSetTitle");
    if (stPos == std::string::npos) {
        fail("INV-2a: RemoteControl::cmdSetTitle definition missing");
    } else {
        std::string body = rc.substr(stPos, 1500);
        if (body.find("isString()") == std::string::npos) {
            fail("INV-2b: cmdSetTitle must validate `title` via isString()");
        }
        if (body.find("isDouble()") == std::string::npos) {
            fail("INV-3a: cmdSetTitle must use isDouble() for `tab` (consistent "
                 "with send-text — keeps `--remote-tab 0` meaningful)");
        }
        if (body.find("currentTabIndexForRemote") == std::string::npos) {
            fail("INV-3b: cmdSetTitle must call currentTabIndexForRemote() when "
                 "`tab` is omitted — fallback resolves to active tab");
        }
    }

    // INV-4: setTabTitleForRemote signature.
    std::regex sig(
        R"(bool\s+setTabTitleForRemote\s*\(\s*int\s+\w+\s*,\s*const\s+QString\s*&\s*\w+\s*\)\s*;)");
    if (!std::regex_search(mwh, sig)) {
        fail("INV-4: MainWindow::setTabTitleForRemote(int, const QString&) "
             "declaration missing — must return bool, non-const");
    }

    // INV-5: titleChanged lambda checks the pin.
    if (mwc.find("m_tabTitlePins.contains") == std::string::npos) {
        fail("INV-5: titleChanged signal handler / updateTabTitles must check "
             "m_tabTitlePins.contains(...) before relabeling — without the guard "
             "the shell's next OSC 0/2 wipes the pin");
    }

    // INV-6: updateTabTitles also checks the pin.
    size_t uttPos = mwc.find("void MainWindow::updateTabTitles");
    if (uttPos == std::string::npos) {
        fail("INV-6a: MainWindow::updateTabTitles definition missing");
    } else {
        std::string body = mwc.substr(uttPos, 1500);
        if (body.find("m_tabTitlePins.contains") == std::string::npos) {
            fail("INV-6b: updateTabTitles must skip pinned tabs — without the "
                 "guard the 2 s tick wipes the pin");
        }
    }

    // INV-7: tab destruction also clears the pin.
    if (mwc.find("m_tabTitlePins.remove(w)") == std::string::npos) {
        fail("INV-7: tab-close path must call m_tabTitlePins.remove(w) so the "
             "QHash<QWidget*, QString> doesn't accumulate dangling keys");
    }

    // INV-8: empty-title path restores via shellTitle() under format "title".
    size_t sttPos = mwc.find("MainWindow::setTabTitleForRemote");
    if (sttPos == std::string::npos) {
        fail("INV-8a: MainWindow::setTabTitleForRemote definition missing");
    } else {
        std::string body = mwc.substr(sttPos, 2500);
        if (body.find("shellTitle()") == std::string::npos) {
            fail("INV-8b: empty-title clear path must call shellTitle() to "
                 "restore the natural label when tabTitleFormat == \"title\" — "
                 "otherwise the cleared pin sits stale until the next OSC 0/2");
        }
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: remote-control `set-title` invariants present\n");
    return 0;
}
