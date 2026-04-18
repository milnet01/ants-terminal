// Command-mark gutter + portal-binding status hint — source-grep regression
// test. See spec.md for the full contract.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_CONFIG_HEADER_PATH
#error "SRC_CONFIG_HEADER_PATH compile definition required"
#endif
#ifndef SRC_CONFIG_IMPL_PATH
#error "SRC_CONFIG_IMPL_PATH compile definition required"
#endif
#ifndef SRC_TERMINALWIDGET_HEADER_PATH
#error "SRC_TERMINALWIDGET_HEADER_PATH compile definition required"
#endif
#ifndef SRC_TERMINALWIDGET_IMPL_PATH
#error "SRC_TERMINALWIDGET_IMPL_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_PATH
#error "SRC_MAINWINDOW_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_HEADER_PATH
#error "SRC_SETTINGSDIALOG_HEADER_PATH compile definition required"
#endif
#ifndef SRC_SETTINGSDIALOG_IMPL_PATH
#error "SRC_SETTINGSDIALOG_IMPL_PATH compile definition required"
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

// Extract the body of a C++ function by scanning for its signature, then
// counting balanced braces forward. We use this instead of a regex with
// a non-greedy `[\s\S]*?^\}` because std::regex's backtracking executor
// blows the stack on a 4500-line source file — 100+k recursion depth
// into _M_dfs. The scanner runs in linear time and constant stack.
static std::string extractFunctionBody(const std::string &src, const std::string &signatureNeedle) {
    auto sigPos = src.find(signatureNeedle);
    if (sigPos == std::string::npos) return {};
    auto bracePos = src.find('{', sigPos);
    if (bracePos == std::string::npos) return {};
    int depth = 1;
    size_t i = bracePos + 1;
    for (; i < src.size() && depth > 0; ++i) {
        char c = src[i];
        if (c == '{') ++depth;
        else if (c == '}') --depth;
    }
    if (depth != 0) return {};
    return src.substr(bracePos, i - bracePos);
}

int main() {
    const std::string cfgHdr = slurp(SRC_CONFIG_HEADER_PATH);
    const std::string cfgImp = slurp(SRC_CONFIG_IMPL_PATH);
    const std::string twHdr  = slurp(SRC_TERMINALWIDGET_HEADER_PATH);
    const std::string twImp  = slurp(SRC_TERMINALWIDGET_IMPL_PATH);
    const std::string mw     = slurp(SRC_MAINWINDOW_PATH);
    const std::string sdHdr  = slurp(SRC_SETTINGSDIALOG_HEADER_PATH);
    const std::string sdImp  = slurp(SRC_SETTINGSDIALOG_IMPL_PATH);
    const std::string cmake  = slurp(SRC_CMAKELISTS_PATH);
    int failures = 0;

    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // (1) Config exposes showCommandMarks with default true. The .toBool(true)
    //     argument is the load-bearing bit — default-off would hide the feature.
    if (!std::regex_search(cfgHdr, std::regex(R"(bool\s+showCommandMarks\s*\(\s*\)\s*const\s*;)")))
        fail("config.h missing bool showCommandMarks() const declaration");
    if (!std::regex_search(cfgHdr, std::regex(R"(void\s+setShowCommandMarks\s*\(\s*bool)")))
        fail("config.h missing setShowCommandMarks declaration");
    if (!std::regex_search(cfgImp, std::regex(
            R"(show_command_marks[\s\S]{0,120}?toBool\s*\(\s*true\s*\))")))
        fail("config.cpp: showCommandMarks must default to true");

    // (2) TerminalWidget exposes the setter + has the member with default true.
    if (!std::regex_search(twHdr, std::regex(R"(void\s+setShowCommandMarks\s*\(\s*bool)")))
        fail("terminalwidget.h missing setShowCommandMarks declaration");
    if (!std::regex_search(twHdr, std::regex(R"(bool\s+m_showCommandMarks\s*=\s*true\s*;)")))
        fail("terminalwidget.h missing m_showCommandMarks default-true member");

    // (3) paintEvent gates on m_showCommandMarks AND non-empty regions.
    //     Scope the search to just the paintEvent body via the balanced-
    //     brace scanner so we don't pay std::regex backtracking cost on
    //     a 4500-line source file.
    const std::string paintBody = extractFunctionBody(twImp, "TerminalWidget::paintEvent");
    if (paintBody.empty()) {
        fail("terminalwidget.cpp paintEvent body not found");
    } else {
        if (paintBody.find("m_showCommandMarks") == std::string::npos)
            fail("paintEvent must gate gutter drawing on m_showCommandMarks");
        if (paintBody.find("regions.empty()") == std::string::npos &&
            paintBody.find("promptRegions().empty()") == std::string::npos)
            fail("paintEvent must early-out when promptRegions().empty()");
        // (4) Gutter positioned just left of the scrollbar.
        if (paintBody.find("m_scrollBar->width()") == std::string::npos)
            fail("gutter painting must subtract m_scrollBar->width() for positioning");
        // (5) Three-state color branch: exit 0, non-zero exit, in-progress.
        //     Two QColor literals inside a commandEndMs > 0 branch (ternary
        //     exitCode == 0 ? green : red) plus one in the else (gray).
        if (paintBody.find("commandEndMs > 0") == std::string::npos ||
            paintBody.find("exitCode == 0") == std::string::npos) {
            fail("gutter color logic must branch on commandEndMs > 0 and exitCode == 0");
        } else {
            // Count QColor occurrences in the paint body — we expect at
            // least 3 (success / failure / in-progress). Allow more in case
            // unrelated painting in the same function uses QColor too.
            size_t pos = 0, count = 0;
            while ((pos = paintBody.find("QColor", pos)) != std::string::npos) {
                ++count;
                ++pos;
            }
            if (count < 3)
                fail("gutter color logic must use at least 3 QColor states (success/failure/in-progress)");
        }
    }

    // (6) MainWindow propagates config to terminals via applyConfigToTerminal.
    if (!std::regex_search(mw, std::regex(
            R"(setShowCommandMarks\s*\(\s*m_config\.showCommandMarks\s*\(\s*\)\s*\))")))
        fail("mainwindow.cpp: applyConfigToTerminal must call setShowCommandMarks(m_config.showCommandMarks())");

    // (7a) SettingsDialog exposes the checkbox + load/save path.
    if (!std::regex_search(sdHdr, std::regex(R"(QCheckBox\s*\*\s*m_showCommandMarks\s*;)")))
        fail("settingsdialog.h missing QCheckBox *m_showCommandMarks");
    if (!std::regex_search(sdImp, std::regex(
            R"(m_showCommandMarks\s*->\s*setChecked\s*\(\s*m_config->showCommandMarks\s*\(\s*\)\s*\))")))
        fail("settingsdialog.cpp: load path must setChecked(m_config->showCommandMarks())");
    if (!std::regex_search(sdImp, std::regex(
            R"(m_config->setShowCommandMarks\s*\(\s*m_showCommandMarks->isChecked\s*\(\s*\)\s*\))")))
        fail("settingsdialog.cpp: save path must call setShowCommandMarks(isChecked())");

    // (7b) Portal status label uses GlobalShortcutsPortal::isAvailable().
    if (!std::regex_search(sdImp, std::regex(
            R"(GlobalShortcutsPortal::isAvailable\s*\(\s*\))")))
        fail("settingsdialog.cpp must branch on GlobalShortcutsPortal::isAvailable()");
    // Both branches must set user-visible text — match the "Portal binding"
    // / "Portal unavailable" strings that users read.
    if (!std::regex_search(sdImp, std::regex(R"(Portal\s+binding\s+active)")))
        fail("settingsdialog.cpp: missing 'Portal binding active' success label");
    if (!std::regex_search(sdImp, std::regex(R"(Portal\s+unavailable)")))
        fail("settingsdialog.cpp: missing 'Portal unavailable' fallback label");

    // (8) CMakeLists.txt wires this target.
    if (!std::regex_search(cmake, std::regex(R"(test_command_mark_gutter)")))
        fail("CMakeLists.txt does not list test_command_mark_gutter target");

    if (failures == 0) {
        std::fprintf(stdout, "OK: command_mark_gutter invariants hold\n");
        return 0;
    }
    std::fprintf(stderr, "command_mark_gutter: %d failure(s)\n", failures);
    return 1;
}
