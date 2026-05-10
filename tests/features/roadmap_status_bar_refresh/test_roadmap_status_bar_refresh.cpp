// ANTS-1160 P2 — status-bar widget refresh-on-launch enforcement.
//
// Source-grep test. Five INVs verifying that refreshRoadmapButton
// and refreshRepoVisibility are wired to BOTH the status-bar timer
// AND the startup singleShot(0) lambda — not only onTabChanged,
// which fires before startShell() sets the PID and so produces
// shellCwd()=="" on the first call.
//
// Spec: tests/features/roadmap_status_bar_refresh/spec.md.
// Source design: docs/specs/ANTS-1160.md §9.
// Plan: docs/plans/ANTS-1160.md P2.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef DOCS_STATUS_BAR_STANDARD_PATH
#error "DOCS_STATUS_BAR_STANDARD_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "FAIL: cannot read " << path << "\n";
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

void must(bool ok, const char *msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

}  // namespace

TEST(RoadmapStatusBarRefresh, Main) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string sb = slurp(DOCS_STATUS_BAR_STANDARD_PATH);

    // INV-1 — refreshRoadmapButton on m_statusTimer
    must(contains(mw,
                  "connect(m_statusTimer, &QTimer::timeout, this, "
                  "&MainWindow::refreshRoadmapButton)"),
         "INV-1: refreshRoadmapButton must be connected to "
         "m_statusTimer via the literal "
         "`connect(m_statusTimer, &QTimer::timeout, this, "
         "&MainWindow::refreshRoadmapButton)` line");

    // INV-3a — refreshRepoVisibility on m_statusTimer
    must(contains(mw,
                  "connect(m_statusTimer, &QTimer::timeout, this, "
                  "&MainWindow::refreshRepoVisibility)"),
         "INV-3a: refreshRepoVisibility must be connected to "
         "m_statusTimer via the literal "
         "`connect(m_statusTimer, &QTimer::timeout, this, "
         "&MainWindow::refreshRepoVisibility)` line");

    // INV-2 + INV-3b — startup singleShot(0) lambda calls both.
    // Locate every `singleShot(0` occurrence; pass if at least one
    // window of ~2000 chars after such an occurrence contains BOTH
    // function names (a single startup lambda often dispatches
    // many refresh calls).
    bool inv2_ok = false;
    bool inv3b_ok = false;
    std::string::size_type pos = 0;
    while ((pos = mw.find("singleShot(0", pos)) != std::string::npos) {
        const std::string window = mw.substr(pos, 2000);
        if (contains(window, "refreshRoadmapButton")) inv2_ok = true;
        if (contains(window, "refreshRepoVisibility")) inv3b_ok = true;
        pos += 1;  // advance past this occurrence
    }
    must(inv2_ok,
         "INV-2: a startup singleShot(0) lambda body must call "
         "refreshRoadmapButton()");
    must(inv3b_ok,
         "INV-3b: a startup singleShot(0) lambda body must call "
         "refreshRepoVisibility()");

    // INV-4 — the existing onTabChanged call chain is preserved
    // (regression-guard; the fix MUST be additive). The chain in
    // mainwindow.cpp is indirect:
    //
    //   onTabChanged() -> refreshStatusBarForActiveTab() ->
    //                     refreshRoadmapButton() + refreshRepoVisibility()
    //
    // Verify each link of the chain by source-grep within a window
    // of the function definition.
    const auto otc = mw.find("void MainWindow::onTabChanged(");
    must(otc != std::string::npos,
         "INV-4: onTabChanged definition must exist (preconditions)");
    const std::string otc_body = mw.substr(otc, 8000);
    must(contains(otc_body, "refreshStatusBarForActiveTab"),
         "INV-4a: onTabChanged must still call "
         "refreshStatusBarForActiveTab (the dispatch to the per-widget "
         "refresh helpers — fix is additive)");

    const auto rsfat = mw.find("void MainWindow::refreshStatusBarForActiveTab(");
    must(rsfat != std::string::npos,
         "INV-4b: refreshStatusBarForActiveTab definition must exist");
    const std::string rsfat_body = mw.substr(rsfat, 8000);
    must(contains(rsfat_body, "refreshRoadmapButton"),
         "INV-4c: refreshStatusBarForActiveTab must call "
         "refreshRoadmapButton (the existing tab-switch dispatch must "
         "still hit the widget — fix is additive, not a replacement)");
    must(contains(rsfat_body, "refreshRepoVisibility"),
         "INV-4d: refreshStatusBarForActiveTab must call "
         "refreshRepoVisibility (the existing tab-switch dispatch must "
         "still hit the widget — fix is additive, not a replacement)");

    // INV-5 — status-bar standard contains the contract addendum
    must(contains(sb, "shellCwd"),
         "INV-5a: status-bar.md mentions shellCwd");
    must(contains(sb, "m_statusTimer"),
         "INV-5b: status-bar.md mentions m_statusTimer");
    must(contains(sb, "singleShot(0)"),
         "INV-5c: status-bar.md mentions singleShot(0)");

    std::cout << "PASS\n";
    return;
}

