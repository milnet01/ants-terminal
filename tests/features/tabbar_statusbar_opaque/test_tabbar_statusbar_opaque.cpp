// Tab bar + status bar opacity source-grep regression test.
// Mirror of test_menubar_hover_stylesheet — same translucent-parent
// failure mode, different bars. See spec.md for invariant rationale.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_PATH
#error "SRC_MAINWINDOW_PATH compile definition required"
#endif

#ifndef SRC_COLOREDTABBAR_CPP_PATH
#error "SRC_COLOREDTABBAR_CPP_PATH compile definition required"
#endif

#ifndef SRC_COLOREDTABBAR_H_PATH
#error "SRC_COLOREDTABBAR_H_PATH compile definition required"
#endif

#ifndef SRC_OPAQUESTATUSBAR_PATH
#error "SRC_OPAQUESTATUSBAR_PATH compile definition required"
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
    const std::string mw   = slurp(SRC_MAINWINDOW_PATH);
    const std::string ctbC = slurp(SRC_COLOREDTABBAR_CPP_PATH);
    const std::string ctbH = slurp(SRC_COLOREDTABBAR_H_PATH);
    const std::string osb  = slurp(SRC_OPAQUESTATUSBAR_PATH);

    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // INV-1: ColoredTabBar exposes setBackgroundFill and the .cpp
    // paintEvent actually calls fillRect. Without fillRect the
    // override is a no-op and the strip paints transparent.
    if (ctbH.find("setBackgroundFill") == std::string::npos) {
        fail("INV-1: ColoredTabBar header missing setBackgroundFill(...) — "
             "applyTheme has no way to push the theme's bgSecondary into "
             "the paintEvent override");
    }
    if (ctbC.find("fillRect") == std::string::npos) {
        fail("INV-1: src/coloredtabbar.cpp does not call fillRect — that's "
             "the actual opaque-paint mechanism. A no-op override would "
             "leave the bar transparent under WA_TranslucentBackground.");
    }

    // INV-2: m_coloredTabBar wired at construction and at applyTheme.
    if (mw.find("m_coloredTabBar->setAttribute(Qt::WA_OpaquePaintEvent, true)")
            == std::string::npos) {
        fail("INV-2: m_coloredTabBar->setAttribute(Qt::WA_OpaquePaintEvent, "
             "true) missing from construction — keeps Qt's region tracking "
             "honest (matches the menubar setup) so dropdown compositor-"
             "damage doesn't flicker on KWin");
    }
    std::regex tabFillCall(
        R"(m_coloredTabBar->setBackgroundFill\s*\()");
    if (!std::regex_search(mw, tabFillCall)) {
        fail("INV-2: applyTheme must call m_coloredTabBar->setBackgroundFill("
             "...) — without it the paintEvent override has no colour and "
             "the bar empty-area renders transparent");
    }

    // INV-3: OpaqueStatusBar header overrides paintEvent and calls
    // fillRect. Source-grep parallel to the OpaqueMenuBar invariant.
    if (osb.find("paintEvent") == std::string::npos ||
        osb.find("fillRect") == std::string::npos) {
        fail("INV-3: src/opaquestatusbar.h must override paintEvent and "
             "call fillRect — that's the actual opaque-paint mechanism. "
             "A no-op subclass would pass the wiring checks below but "
             "still show the desktop through the status bar.");
    }
    if (osb.find("setBackgroundFill") == std::string::npos) {
        fail("INV-3: OpaqueStatusBar must expose setBackgroundFill(...) — "
             "applyTheme pushes the theme's bgSecondary through this entry "
             "point");
    }

    // INV-4: m_statusBar constructed as OpaqueStatusBar AND installed
    // via setStatusBar before the first statusBar() call. Without the
    // explicit setStatusBar, Qt's QMainWindow lazy-creates a plain
    // QStatusBar on first access — once that happens, replacing it
    // doesn't help the frames already painted.
    std::regex opaqueCtor(
        R"(m_statusBar\s*=\s*new\s+OpaqueStatusBar\s*\()");
    if (!std::regex_search(mw, opaqueCtor)) {
        fail("INV-4: m_statusBar must be constructed as `new OpaqueStatusBar"
             "(...)` — a plain QStatusBar paints transparent under "
             "WA_TranslucentBackground on KWin/Breeze");
    }
    if (mw.find("setStatusBar(m_statusBar)") == std::string::npos) {
        fail("INV-4: setStatusBar(m_statusBar) call missing — without it Qt "
             "lazy-creates a plain QStatusBar on the first statusBar() "
             "access and the OpaqueStatusBar pointer is orphaned");
    }
    std::regex statusFillCall(
        R"(m_statusBar->setBackgroundFill\s*\()");
    if (!std::regex_search(mw, statusFillCall)) {
        fail("INV-4: applyTheme must call m_statusBar->setBackgroundFill("
             "...) — without it the OpaqueStatusBar paintEvent has no "
             "colour and falls back to transparent");
    }

    // INV-5: paint order. The fillRect must precede QTabBar::paintEvent;
    // if the base class paints first the fill paints over the tabs.
    // Locate both anchors in coloredtabbar.cpp and assert ordering.
    const std::string::size_type fillPos =
        ctbC.find("fillRect");
    const std::string::size_type basePos =
        ctbC.find("QTabBar::paintEvent");
    if (fillPos == std::string::npos || basePos == std::string::npos) {
        fail("INV-5: coloredtabbar.cpp must contain both fillRect and "
             "QTabBar::paintEvent — paint-order check requires both anchors");
    } else if (fillPos >= basePos) {
        fail("INV-5: fillRect must appear BEFORE QTabBar::paintEvent in "
             "ColoredTabBar::paintEvent — otherwise the base class draws "
             "tabs first and the fill paints over them, blanking the bar");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see spec.md for context\n", failures);
        return 1;
    }
    std::printf("OK: tab bar + status bar opacity invariants present\n");
    return 0;
}
