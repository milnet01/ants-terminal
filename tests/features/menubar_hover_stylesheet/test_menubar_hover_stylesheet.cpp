// Menubar hover stylesheet — no-flicker source-grep regression test.
// See spec.md for the full contract.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_PATH
#error "SRC_MAINWINDOW_PATH compile definition required"
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
    const std::string mw = slurp(SRC_MAINWINDOW_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: autoFillBackground(true) on the menubar.
    if (mw.find("m_menuBar->setAutoFillBackground(true)") == std::string::npos) {
        fail("INV-1: m_menuBar->setAutoFillBackground(true) missing — "
             "required so QMenuBar paints an opaque background under a "
             "WA_TranslucentBackground parent");
    }

    // INV-2: WA_StyledBackground on the menubar.
    std::regex styledBg(
        R"(m_menuBar->setAttribute\s*\(\s*Qt::WA_StyledBackground\s*,\s*true\s*\))");
    if (!std::regex_search(mw, styledBg)) {
        fail("INV-2: Qt::WA_StyledBackground on m_menuBar missing — QSS "
             "background-color will not paint reliably under a translucent parent");
    }

    // INV-3: QMenuBar::item:hover stylesheet rule with a non-transparent
    // background. Without it, the closed-menu hover flash returns (the
    // one-frame :hover-before-:selected gap on Breeze / Fusion).
    std::regex hoverRule(
        R"(QMenuBar::item:hover\s*\{\s*background-color:\s*%\d+\s*;)");
    if (!std::regex_search(mw, hoverRule)) {
        fail("INV-3: QSS block is missing a QMenuBar::item:hover rule — "
             "re-opens the closed-menu one-frame hover flash (2026-04-20 "
             "original report). The dropdown-flicker co-regression is fixed "
             "by WA_OpaquePaintEvent on the menubar, not by dropping this rule.");
    }

    // INV-3b: WA_OpaquePaintEvent set on m_menuBar at construction.
    // The menubar's autoFillBackground + QSS fully cover every pixel on
    // every paint, so marking it opaque-on-paint stops Qt from
    // invalidating the translucent parent's compositor region under it.
    // Without this, each mouse-move over the menubar item owning an
    // open dropdown damages the popup above the menubar on KWin. The
    // terminal_partial_update_mode test covers a related but separate
    // dropdown-flicker cause (the TerminalWidget QOpenGLWidget base
    // class that drove ~60 Hz full-window UpdateRequests on MainWindow);
    // that fix is orthogonal, so we assert the menubar attribute here.
    if (mw.find("m_menuBar->setAttribute(Qt::WA_OpaquePaintEvent, true)")
            == std::string::npos) {
        fail("INV-3b: m_menuBar->setAttribute(Qt::WA_OpaquePaintEvent, true) "
             "missing from construction site — reopens the "
             "mouse-move-over-menubar compositor damage that flickers the "
             "open dropdown popup on KWin");
    }

    // INV-4: QMenuBar::item base rule (0.6.43) still present.
    std::regex baseItemRule(
        R"(QMenuBar::item\s*\{\s*background-color:\s*transparent)");
    if (!std::regex_search(mw, baseItemRule)) {
        fail("INV-4: QMenuBar::item base rule removed — Qt will fall back "
             "to native rendering for the non-selected state (0.6.43 regression)");
    }

    // INV-5: setNativeMenuBar(false).
    if (mw.find("m_menuBar->setNativeMenuBar(false)") == std::string::npos) {
        fail("INV-5: setNativeMenuBar(false) missing — a global-menu DE "
             "integration would hide the menubar inside our frameless window");
    }

    // INV-6 (negative): do NOT port the QMenuBar::item transparent-
    // background base rule into QMenu::item. The dropdown is a
    // separate top-level popup with its own (non-translucent) window;
    // adding a transparent-base rule there introduced visible redraws
    // as the current item shifted rows on mouse move. If you're
    // tempted to add parallelism with the QMenuBar rule, re-read the
    // 2026-04-20 user report first.
    std::regex qmenuTransparentBase(
        R"(QMenu::item\s*\{\s*background-color:\s*transparent)");
    if (std::regex_search(mw, qmenuTransparentBase)) {
        fail("INV-6 (neg): QMenu::item must NOT have an explicit "
             "transparent-background base rule — it re-opens the dropdown-"
             "redraw flicker reported 2026-04-20");
    }

    if (failures > 0) {
        std::fprintf(stderr, "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: menubar hover stylesheet invariants present\n");
    return 0;
}
