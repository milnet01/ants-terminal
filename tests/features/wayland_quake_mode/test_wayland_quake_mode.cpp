// Wayland-native Quake mode — see spec.md.
//
// Source-grep regression test pinning the 0.6.38 invariants:
//   (1) LayerShellQt include is guarded by ANTS_WAYLAND_LAYER_SHELL.
//   (2) setupQuakeMode dispatches Wayland vs X11 at runtime.
//   (3) toggleQuakeVisibility skips the pos() animation on Wayland.
//   (4) quake_hotkey config key is wired to a QShortcut invoking
//       toggleQuakeVisibility.
//   (5) CMakeLists.txt uses find_package(LayerShellQt CONFIG QUIET).

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

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
    const std::string mw = slurp(SRC_MAINWINDOW_PATH);
    const std::string cmake = slurp(SRC_CMAKELISTS_PATH);
    int failures = 0;

    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // (1) The LayerShellQt include must sit inside an
    //     `#ifdef ANTS_WAYLAND_LAYER_SHELL` block. An un-guarded include
    //     would break the build on hosts without the devel package.
    std::regex guardedInclude(
        R"(#ifdef\s+ANTS_WAYLAND_LAYER_SHELL\s*\n\s*#include\s*<LayerShellQt/Window>)");
    if (!std::regex_search(mw, guardedInclude)) {
        fail("<LayerShellQt/Window> include is not guarded by "
             "#ifdef ANTS_WAYLAND_LAYER_SHELL in src/mainwindow.cpp");
    }

    // (2) setupQuakeMode must do a runtime platformName() check AND the
    //     layer-shell setup block must be guarded. The X11 branch
    //     (WindowStaysOnTopHint + Qt::Tool) must still exist outside
    //     the Wayland conditional.
    std::regex setupFn(R"(void\s+MainWindow::setupQuakeMode\s*\(\)\s*\{)");
    auto it = std::sregex_iterator(mw.begin(), mw.end(), setupFn);
    if (it == std::sregex_iterator()) {
        fail("setupQuakeMode() definition not found in mainwindow.cpp");
    } else {
        // Extract the function body — scan forward to the matching close-brace
        // at depth 0. Good-enough brace counter for a single well-formed
        // function (no raw-string literals in this body).
        size_t start = it->position() + it->length();
        int depth = 1;
        size_t i = start;
        for (; i < mw.size() && depth > 0; ++i) {
            if (mw[i] == '{') ++depth;
            else if (mw[i] == '}') --depth;
        }
        const std::string body = mw.substr(start, i - start);

        // Must do a runtime platformName() Wayland check.
        if (body.find("QGuiApplication::platformName()") == std::string::npos) {
            fail("setupQuakeMode() missing runtime platformName() check");
        }
        if (body.find("wayland") == std::string::npos &&
            body.find("Wayland") == std::string::npos) {
            fail("setupQuakeMode() missing wayland-string comparison");
        }

        // Must still contain the X11-path WindowStaysOnTopHint + Qt::Tool
        // flags. The 0.6.38 refactor kept the X11 branch verbatim; a
        // regression that moves the flag set inside the Wayland check
        // (or deletes it) would strand X11 users.
        if (body.find("WindowStaysOnTopHint") == std::string::npos) {
            fail("setupQuakeMode() lost the WindowStaysOnTopHint (X11 path broken)");
        }
        if (body.find("Qt::Tool") == std::string::npos) {
            fail("setupQuakeMode() lost Qt::Tool flag (X11 path broken)");
        }

        // The layer-shell block must be guarded.
        if (body.find("ANTS_WAYLAND_LAYER_SHELL") == std::string::npos) {
            fail("setupQuakeMode() has no ANTS_WAYLAND_LAYER_SHELL guard");
        }
        // If layer-shell is set up, the LayerTop layer + an AnchorTop
        // flag are the minimum for a Quake dropdown. Missing either
        // means the window wouldn't stack above other surfaces or
        // would float wherever the compositor decides.
        if (body.find("LayerTop") == std::string::npos) {
            fail("layer-shell block missing LayerTop — dropdown would sink below other surfaces");
        }
        if (body.find("AnchorTop") == std::string::npos) {
            fail("layer-shell block missing AnchorTop — dropdown wouldn't anchor to screen top");
        }
    }

    // (3) toggleQuakeVisibility must skip the pos() animation on Wayland.
    //     Regex: look for an isWayland short-circuit ahead of the
    //     QPropertyAnimation setup.
    std::regex toggleFn(R"(void\s+MainWindow::toggleQuakeVisibility\s*\(\)\s*\{)");
    it = std::sregex_iterator(mw.begin(), mw.end(), toggleFn);
    if (it == std::sregex_iterator()) {
        fail("toggleQuakeVisibility() definition not found");
    } else {
        size_t start = it->position() + it->length();
        int depth = 1;
        size_t i = start;
        for (; i < mw.size() && depth > 0; ++i) {
            if (mw[i] == '{') ++depth;
            else if (mw[i] == '}') --depth;
        }
        const std::string body = mw.substr(start, i - start);

        if (body.find("platformName()") == std::string::npos) {
            fail("toggleQuakeVisibility() missing platformName() check — "
                 "Wayland users get a broken animation");
        }
        // The Wayland branch must return before the QPropertyAnimation
        // setup. Cheapest proof: "return" appears before "QPropertyAnimation".
        size_t animPos = body.find("QPropertyAnimation");
        size_t returnPos = body.find("return;");
        if (animPos != std::string::npos &&
            (returnPos == std::string::npos || returnPos > animPos)) {
            fail("toggleQuakeVisibility() animation not skipped on Wayland — "
                 "expected an early `return;` before the QPropertyAnimation block");
        }
    }

    // (4) quake_hotkey wiring: the constructor must build a QShortcut from
    //     Config::quakeHotkey() and connect it to toggleQuakeVisibility.
    //     Pre-0.6.38 the config key was saved by Settings but never read.
    std::regex wiring(
        R"(quakeHotkey\s*\(\s*\)[\s\S]{0,400}?QShortcut[\s\S]{0,400}?toggleQuakeVisibility)");
    if (!std::regex_search(mw, wiring)) {
        fail("quake_hotkey → QShortcut → toggleQuakeVisibility wire missing "
             "(the config key is dead without this)");
    }

    // (5) CMakeLists.txt uses find_package(LayerShellQt CONFIG QUIET).
    //     Without QUIET the build prints a scary warning on every host
    //     lacking the devel package.
    std::regex findPkg(
        R"(find_package\s*\(\s*LayerShellQt\s+CONFIG\s+QUIET\s*\))");
    if (!std::regex_search(cmake, findPkg)) {
        fail("CMakeLists.txt: find_package(LayerShellQt CONFIG QUIET) not found "
             "(missing QUIET means noisy warnings on distros without devel)");
    }
    // And when found, the LayerShellQt::Interface target must be linked
    // to ants-terminal.
    std::regex linkTarget(
        R"(target_link_libraries\s*\(\s*ants-terminal\s+PRIVATE\s+LayerShellQt::Interface\s*\))");
    if (!std::regex_search(cmake, linkTarget)) {
        fail("CMakeLists.txt does not link LayerShellQt::Interface to "
             "ants-terminal under if(LayerShellQt_FOUND)");
    }

    if (failures) {
        std::fprintf(stderr, "\n%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("wayland_quake_mode: OK\n");
    return 0;
}
