// Freedesktop Portal GlobalShortcuts — source-grep regression test.
// See spec.md for the full contract. Follows the same pattern as
// wayland_quake_mode / threaded_ptywrite_gating / shift_enter_bracketed_paste.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#ifndef SRC_PORTAL_HEADER_PATH
#error "SRC_PORTAL_HEADER_PATH compile definition required"
#endif
#ifndef SRC_PORTAL_IMPL_PATH
#error "SRC_PORTAL_IMPL_PATH compile definition required"
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
    const std::string hdr   = slurp(SRC_PORTAL_HEADER_PATH);
    const std::string impl  = slurp(SRC_PORTAL_IMPL_PATH);
    const std::string mw    = slurp(SRC_MAINWINDOW_PATH);
    const std::string cmake = slurp(SRC_CMAKELISTS_PATH);
    int failures = 0;

    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    // (1) MainWindow gates portal construction on isAvailable(). Without
    //     this, a missing xdg-desktop-portal service produces a pending
    //     call that never completes (silent hang + watcher leak).
    std::regex gateRegex(
        R"(GlobalShortcutsPortal::isAvailable\s*\(\s*\)[\s\S]{0,400}?new\s+GlobalShortcutsPortal\s*\()");
    if (!std::regex_search(mw, gateRegex)) {
        fail("MainWindow creates GlobalShortcutsPortal without "
             "GlobalShortcutsPortal::isAvailable() gate");
    }

    // (2) Canonical D-Bus identifiers must appear verbatim in the impl
    //     (typoed service name = silent no-op; portal spec is load-bearing).
    auto requireLiteral = [&](const std::string &needle, const char *label) {
        if (impl.find(needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL: globalshortcutsportal.cpp missing %s "
                "literal \"%s\"\n", label, needle.c_str());
            ++failures;
        }
    };
    requireLiteral("org.freedesktop.portal.Desktop",         "service name");
    requireLiteral("/org/freedesktop/portal/desktop",        "portal object path");
    requireLiteral("org.freedesktop.portal.GlobalShortcuts", "interface name");
    requireLiteral("org.freedesktop.portal.Request",         "request interface");
    requireLiteral("CreateSession",                          "CreateSession method");
    requireLiteral("BindShortcuts",                          "BindShortcuts method");
    requireLiteral("Activated",                              "Activated signal");

    // (3) MainWindow binds with the literal id "toggle-quake" AND matches
    //     on the same id in the Activated handler. Id drift between bind
    //     and match is a silent failure we want loudly pinned.
    std::regex bindId(
        R"(bindShortcut\s*\(\s*QStringLiteral\s*\(\s*\"toggle-quake\"\s*\))");
    if (!std::regex_search(mw, bindId)) {
        fail("MainWindow does not call bindShortcut(\"toggle-quake\", ...)");
    }
    // The activated lambda must filter on the same id.
    std::regex matchId(
        R"(GlobalShortcutsPortal::activated[\s\S]{0,400}?\"toggle-quake\")");
    if (!std::regex_search(mw, matchId)) {
        fail("Activated handler in MainWindow doesn't match id \"toggle-quake\" — "
             "portal events for the right shortcut would be silently dropped");
    }

    // (4) The in-app QShortcut must stay wired even on the portal path.
    //     Defence-in-depth for GNOME Shell (no GlobalShortcuts impl yet)
    //     and for the window between CreateSession and BindShortcuts
    //     response. Pattern: QShortcut creation appears inside the
    //     quakeMode constructor branch regardless of portal availability.
    std::regex sc(
        R"(quakeHotkey\s*\(\s*\)[\s\S]{0,600}?new\s+QShortcut)");
    if (!std::regex_search(mw, sc)) {
        fail("In-app QShortcut fallback removed or moved outside the "
             "quakeHotkey()-reading branch — 0.6.38 behaviour regressed");
    }

    // (5) Both paths debounce via m_lastQuakeToggleMs. Without debounce,
    //     a focused double-fire would hide-then-show the window.
    if (mw.find("m_lastQuakeToggleMs") == std::string::npos) {
        fail("m_lastQuakeToggleMs debounce field not referenced in mainwindow.cpp");
    }
    // Both the QShortcut lambda and the portal lambda need the read+write.
    std::regex dbPattern(R"(m_lastQuakeToggleMs\s*<\s*500)");
    auto debounceCount = std::distance(
        std::sregex_iterator(mw.begin(), mw.end(), dbPattern),
        std::sregex_iterator());
    if (debounceCount < 2) {
        fail("m_lastQuakeToggleMs debounce missing on one of the two "
             "activation paths (QShortcut + portal) — expected >=2 sites, "
             "found fewer");
    }

    // (6) CMakeLists.txt: source is listed + Qt6::DBus is linked + source
    //     sits next to other src/*.cpp sources in the ants-terminal target.
    std::regex src(R"(src/globalshortcutsportal\.cpp)");
    if (!std::regex_search(cmake, src)) {
        fail("CMakeLists.txt: src/globalshortcutsportal.cpp not added to "
             "ants-terminal sources");
    }
    std::regex dbusLink(R"(Qt6::DBus)");
    if (!std::regex_search(cmake, dbusLink)) {
        fail("CMakeLists.txt: Qt6::DBus link missing — required by "
             "globalshortcutsportal.cpp");
    }

    // (7) Header establishes the class name and the required public
    //     signal/slot surface. If this drifts, MainWindow's connect() on
    //     the class compile-breaks and we catch it earlier.
    if (hdr.find("class GlobalShortcutsPortal") == std::string::npos) {
        fail("globalshortcutsportal.h: class declaration missing/renamed");
    }
    if (hdr.find("void activated(") == std::string::npos) {
        fail("globalshortcutsportal.h: activated(QString) signal missing");
    }
    if (hdr.find("bindShortcut(") == std::string::npos) {
        fail("globalshortcutsportal.h: bindShortcut() public method missing");
    }
    if (hdr.find("isAvailable()") == std::string::npos) {
        fail("globalshortcutsportal.h: isAvailable() static method missing");
    }

    if (failures) {
        std::fprintf(stderr, "\n%d assertion(s) failed\n", failures);
        return 1;
    }
    std::printf("global_shortcuts_portal: OK\n");
    return 0;
}
