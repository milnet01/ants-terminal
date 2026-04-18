// OSC 133 "last completed command" actions — source-grep regression test.
// See spec.md for the full contract. Follows the same pattern as
// wayland_quake_mode / global_shortcuts_portal / shift_enter_bracketed_paste.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_TERMINALWIDGET_HEADER_PATH
#error "SRC_TERMINALWIDGET_HEADER_PATH compile definition required"
#endif
#ifndef SRC_TERMINALWIDGET_IMPL_PATH
#error "SRC_TERMINALWIDGET_IMPL_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_PATH
#error "SRC_MAINWINDOW_PATH compile definition required"
#endif
#ifndef SRC_CMAKELISTS_PATH
#error "SRC_CMAKELISTS_PATH compile definition required"
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
    const std::string hdr   = slurp(SRC_TERMINALWIDGET_HEADER_PATH);
    const std::string impl  = slurp(SRC_TERMINALWIDGET_IMPL_PATH);
    const std::string mw    = slurp(SRC_MAINWINDOW_PATH);
    const std::string cmake = slurp(SRC_CMAKELISTS_PATH);
    int failures = 0;

    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // (1) Header declares both slots in the public API.
    if (!std::regex_search(hdr, std::regex(R"(int\s+copyLastCommandOutput\s*\(\s*\)\s*;)")))
        fail("terminalwidget.h missing int copyLastCommandOutput() declaration");
    if (!std::regex_search(hdr, std::regex(R"(int\s+rerunLastCommand\s*\(\s*\)\s*;)")))
        fail("terminalwidget.h missing int rerunLastCommand() declaration");

    // (2) Both methods walk promptRegions backwards and gate on
    //     commandEndMs > 0. The loop shape we expect: `for (int i = ...
    //     size() - 1; i >= 0; --i)` followed within ~400 chars by a
    //     `commandEndMs > 0` comparison. This catches the "skip the
    //     in-progress tail" invariant without pinning us to an exact
    //     variable name.
    std::regex tailSkipRegex(
        R"(for\s*\(\s*int\s+\w+\s*=\s*static_cast<int>\s*\(\s*regions\.size\s*\(\s*\)\s*\)\s*-\s*1[\s\S]{0,400}?commandEndMs\s*>\s*0)");
    auto countBackwardLoops = [&](const std::string &s) {
        auto begin = std::sregex_iterator(s.begin(), s.end(), tailSkipRegex);
        auto end = std::sregex_iterator();
        return std::distance(begin, end);
    };
    // Scope the search to just our two new methods so we don't accidentally
    // match navigatePrompt's similar backwards loop.
    std::smatch copyBlock;
    std::regex copyMethodRegex(
        R"(int\s+TerminalWidget::copyLastCommandOutput\s*\([\s\S]*?^\})",
        std::regex::multiline);
    std::regex rerunMethodRegex(
        R"(int\s+TerminalWidget::rerunLastCommand\s*\([\s\S]*?^\})",
        std::regex::multiline);
    if (!std::regex_search(impl, copyBlock, copyMethodRegex)) {
        fail("terminalwidget.cpp missing copyLastCommandOutput definition");
    } else if (countBackwardLoops(copyBlock.str()) < 1) {
        fail("copyLastCommandOutput must walk regions backwards gated on commandEndMs > 0");
    }
    std::smatch rerunBlock;
    if (!std::regex_search(impl, rerunBlock, rerunMethodRegex)) {
        fail("terminalwidget.cpp missing rerunLastCommand definition");
    } else if (countBackwardLoops(rerunBlock.str()) < 1) {
        fail("rerunLastCommand must walk regions backwards gated on commandEndMs > 0");
    }

    // (3) copyLastCommandOutput delegates to outputTextAt, NOT
    //     lastCommandOutput (which caps at 100 lines for trigger noise).
    if (copyBlock.size()) {
        const std::string body = copyBlock.str();
        if (!std::regex_search(body, std::regex(R"(outputTextAt\s*\()")))
            fail("copyLastCommandOutput must use outputTextAt for unbounded output");
        if (std::regex_search(body, std::regex(R"(lastCommandOutput\s*\()")))
            fail("copyLastCommandOutput must NOT call lastCommandOutput (100-line cap)");
    }

    // (4) MainWindow wires both actions through config.keybinding with the
    //     expected keys + defaults. Pinning the defaults to Ctrl+Alt+O/R is
    //     deliberate: both are unused elsewhere in mainwindow.cpp.
    if (!std::regex_search(mw, std::regex(
            R"(m_config\.keybinding\s*\(\s*"copy_last_output"\s*,\s*"Ctrl\+Alt\+O"\s*\))")))
        fail("mainwindow.cpp: copy_last_output must default to Ctrl+Alt+O via config.keybinding");
    if (!std::regex_search(mw, std::regex(
            R"(m_config\.keybinding\s*\(\s*"rerun_last_command"\s*,\s*"Ctrl\+Alt\+R"\s*\))")))
        fail("mainwindow.cpp: rerun_last_command must default to Ctrl+Alt+R via config.keybinding");

    // (5) Defaults don't collide with other bindings — a second Ctrl+Alt+O
    //     or Ctrl+Alt+R would mean we're shadowing an existing feature.
    auto countOccurrences = [&](const std::string &s, const std::string &pat) {
        std::regex r(pat);
        return std::distance(
            std::sregex_iterator(s.begin(), s.end(), r),
            std::sregex_iterator());
    };
    if (countOccurrences(mw, R"("Ctrl\+Alt\+O")") != 1)
        fail("Ctrl+Alt+O must appear exactly once in mainwindow.cpp (ours); another binding shadows it");
    if (countOccurrences(mw, R"("Ctrl\+Alt\+R")") != 1)
        fail("Ctrl+Alt+R must appear exactly once in mainwindow.cpp (ours); another binding shadows it");

    // (6) MainWindow surfaces status-bar feedback on both success and
    //     failure for each action.
    if (!std::regex_search(mw, std::regex(
            R"(copyLastCommandOutput\s*\(\s*\)[\s\S]{0,600}?showStatusMessage[\s\S]{0,400}?showStatusMessage)")))
        fail("mainwindow.cpp: copy_last_output handler missing both success + failure toasts");
    if (!std::regex_search(mw, std::regex(
            R"(rerunLastCommand\s*\(\s*\)[\s\S]{0,400}?showStatusMessage)")))
        fail("mainwindow.cpp: rerun_last_command handler missing failure toast");

    // (7) CMakeLists.txt wires this test target.
    if (!std::regex_search(cmake, std::regex(R"(test_osc133_last_command)")))
        fail("CMakeLists.txt does not list test_osc133_last_command target");

    if (failures == 0) {
        std::fprintf(stdout, "OK: osc133_last_command invariants hold\n");
        return 0;
    }
    std::fprintf(stderr, "osc133_last_command: %d failure(s)\n", failures);
    return 1;
}
