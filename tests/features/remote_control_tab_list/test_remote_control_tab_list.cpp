// Feature-conformance test for tests/features/remote_control_tab_list/spec.md.
//
// Source-grep harness — verifies the `tab-list` verb is registered,
// `MainWindow::tabsAsJson` is wired, and the per-tab JSON shape
// includes the six expected keys. Behavioural drive of `tabsAsJson`
// would require a real MainWindow + tab widget tree; out-of-scope for
// the v1 source-grep tier.
//
// Exit 0 = all 7 invariants hold.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H
#error "SRC_MAINWINDOW_H compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP
#error "SRC_MAINWINDOW_CPP compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

int main() {
    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcSrc = slurp(SRC_RC_CPP);
    const std::string mwHdr = slurp(SRC_MAINWINDOW_H);
    const std::string mwSrc = slurp(SRC_MAINWINDOW_CPP);

    if (rcHdr.empty()) return fail("INV-2", "remotecontrol.h not readable");
    if (rcSrc.empty()) return fail("INV-2", "remotecontrol.cpp not readable");
    if (mwHdr.empty()) return fail("INV-3", "mainwindow.h not readable");
    if (mwSrc.empty()) return fail("INV-3", "mainwindow.cpp not readable");

    // INV-1: dispatch registers tab-list.
    if (!contains(rcSrc, "QLatin1String(\"tab-list\")"))
        return fail("INV-1", "dispatch missing tab-list branch");

    // INV-2: cmdTabList declared + defined.
    if (!contains(rcHdr, "cmdTabList"))
        return fail("INV-2", "cmdTabList not declared in remotecontrol.h");
    if (!contains(rcSrc, "RemoteControl::cmdTabList"))
        return fail("INV-2", "cmdTabList handler body missing");

    // INV-3: tabsAsJson declared + defined.
    if (!contains(mwHdr, "tabsAsJson"))
        return fail("INV-3", "tabsAsJson not declared in mainwindow.h");
    if (!contains(mwSrc, "MainWindow::tabsAsJson"))
        return fail("INV-3", "tabsAsJson body missing");

    // INV-4: per-tab JSON includes the six expected keys.
    const char *expectedKeys[] = {
        "\"index\"",
        "\"title\"",
        "\"cwd\"",
        "\"shell_pid\"",
        "\"claude_running\"",
        "\"color\"",
    };
    for (const char *k : expectedKeys) {
        if (!contains(mwSrc, k))
            return fail("INV-4", k);
    }

    // INV-5: claude_running consults ClaudeTabTracker::shellState(pid)
    // and compares against ClaudeState::NotRunning. Match the call
    // shape rather than the exact text, so a future style tweak that
    // splits the comparison across lines doesn't break the invariant.
    if (!contains(mwSrc, "m_claudeTabTracker->shellState"))
        return fail("INV-5",
                    "tabsAsJson must consult m_claudeTabTracker->shellState");
    if (!contains(mwSrc, "ClaudeState::NotRunning"))
        return fail("INV-5",
                    "tabsAsJson must compare against ClaudeState::NotRunning");

    // INV-6: response envelope includes both ok:true and tabs.
    if (!contains(rcSrc, "out[\"tabs\"] = m_main->tabsAsJson()"))
        return fail("INV-6",
                    "cmdTabList must return tabs via tabsAsJson()");
    if (!contains(rcSrc, "out[\"ok\"] = true"))
        return fail("INV-6", "cmdTabList must set ok:true");

    // INV-7: tabListForRemote untouched (the existing ls surface).
    // We don't compare line-by-line; we just confirm the function still
    // exists in mainwindow.cpp — the regression we'd want to catch is
    // accidental deletion / merge into tabsAsJson.
    if (!contains(mwSrc, "MainWindow::tabListForRemote"))
        return fail("INV-7",
                    "tabListForRemote must remain (additive, not replacing)");

    std::puts("OK remote_control_tab_list: 7/7 invariants");
    return 0;
}
