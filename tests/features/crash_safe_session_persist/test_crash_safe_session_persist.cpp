// Feature-conformance test for ANTS-1159 — crash-safe session
// persistence wiring. Pure source-grep (no link).
//
// INV labels qualified ANTS-1159-INV-N. See spec.md.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const std::string &detail = {}) {
    std::fprintf(stderr, "[%-72s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 detail.empty() ? "" : " — ",
                 detail.c_str());
    if (!ok) ++g_failures;
}

std::string readFile(const char *path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Brace-balanced extraction (same shape as ui_state_persistence).
std::string extractBody(const std::string &source, const std::string &sig) {
    auto sigPos = source.find(sig);
    if (sigPos == std::string::npos) return {};
    auto open = source.find('{', sigPos);
    if (open == std::string::npos) return {};
    int depth = 1;
    auto pos = open + 1;
    while (pos < source.size() && depth > 0) {
        if (source[pos] == '{') ++depth;
        else if (source[pos] == '}') --depth;
        ++pos;
    }
    return source.substr(sigPos, pos - sigPos);
}

void testInv1_timerMemberDeclaredAndInitialised() {
    const std::string h  = readFile("src/mainwindow.h");
    const std::string cp = readFile("src/mainwindow.cpp");
    if (h.empty() || cp.empty()) {
        expect(false, "ANTS-1159-INV-1: read mainwindow.{h,cpp}");
        return;
    }

    expect(contains(h, "m_sessionSaveTimer"),
           "ANTS-1159-INV-1: m_sessionSaveTimer declared in mainwindow.h");
    // 30000 ms must appear near the timer setup. Looser grep:
    // either "setInterval(30000)" or "->start(30000)" or "m_sessionSaveTimer"
    // co-located with the literal somewhere in mainwindow.cpp.
    const bool hasInterval =
        contains(cp, "30000")
        && (contains(cp, "m_sessionSaveTimer") || contains(cp, "sessionSaveTimer"));
    expect(hasInterval,
           "ANTS-1159-INV-1: mainwindow.cpp configures m_sessionSaveTimer with 30000 ms");
}

void testInv2_timerConnectedToSaveAllSessions() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-2: read mainwindow.cpp"); return; }

    // connect(m_sessionSaveTimer, &QTimer::timeout, this, &MainWindow::saveAllSessions)
    // — accept either lambda or pmf form. Look for "m_sessionSaveTimer" near
    // "saveAllSessions" within ~200 chars.
    const auto stPos = cp.find("m_sessionSaveTimer");
    bool wired = false;
    if (stPos != std::string::npos) {
        // Walk the file from each occurrence of m_sessionSaveTimer; if any
        // occurrence sits in a connect() that mentions saveAllSessions
        // within 200 chars on either side, the wiring is present.
        std::size_t i = 0;
        while ((i = cp.find("m_sessionSaveTimer", i)) != std::string::npos) {
            const std::size_t lo = i > 200 ? i - 200 : 0;
            const std::string near = cp.substr(lo, 400);
            if (contains(near, "saveAllSessions") && contains(near, "connect(")) {
                wired = true;
                break;
            }
            i += 1;
        }
    }
    expect(wired,
           "ANTS-1159-INV-2: m_sessionSaveTimer::timeout connected to saveAllSessions");
}

void testInv3_saveTabOrderOnlyDeclaredAndDefined() {
    const std::string h  = readFile("src/mainwindow.h");
    const std::string cp = readFile("src/mainwindow.cpp");
    if (h.empty() || cp.empty()) { expect(false, "ANTS-1159-INV-3: read mainwindow.{h,cpp}"); return; }

    expect(contains(h, "saveTabOrderOnly"),
           "ANTS-1159-INV-3: saveTabOrderOnly declared in mainwindow.h");
    expect(contains(cp, "MainWindow::saveTabOrderOnly")
            || contains(cp, "saveTabOrderOnly("),
           "ANTS-1159-INV-3: saveTabOrderOnly defined in mainwindow.cpp");
}

void testInv4_calledFromNewTab() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-4: read mainwindow.cpp"); return; }

    const std::string body = extractBody(cp, "void MainWindow::newTab()");
    expect(!body.empty(), "ANTS-1159-INV-4: newTab body extracted");
    expect(contains(body, "saveTabOrderOnly"),
           "ANTS-1159-INV-4: newTab calls saveTabOrderOnly");
}

void testInv5_calledFromPerformTabClose() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-5: read mainwindow.cpp"); return; }

    const std::string body = extractBody(cp, "void MainWindow::performTabClose");
    expect(!body.empty(), "ANTS-1159-INV-5: performTabClose body extracted");
    expect(contains(body, "saveTabOrderOnly"),
           "ANTS-1159-INV-5: performTabClose calls saveTabOrderOnly");
}

void testInv6_connectedToTabMoved() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-6: read mainwindow.cpp"); return; }

    // Look for a connect() mentioning tabMoved AND saveTabOrderOnly within
    // 300 chars.
    const auto tmPos = cp.find("tabMoved");
    bool wired = false;
    if (tmPos != std::string::npos) {
        std::size_t i = 0;
        while ((i = cp.find("tabMoved", i)) != std::string::npos) {
            const std::size_t lo = i > 300 ? i - 300 : 0;
            const std::string near = cp.substr(lo, 600);
            if (contains(near, "saveTabOrderOnly") && contains(near, "connect(")) {
                wired = true;
                break;
            }
            i += 1;
        }
    }
    expect(wired,
           "ANTS-1159-INV-6: tabMoved signal connected to saveTabOrderOnly");
}

void testInv7_closeEventStopsTimer() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-7: read mainwindow.cpp"); return; }

    const std::string body = extractBody(cp, "void MainWindow::closeEvent");
    expect(!body.empty(), "ANTS-1159-INV-7: closeEvent body extracted");

    // The body must call ->stop() on m_sessionSaveTimer BEFORE saveAllSessions().
    const auto stopPos = body.find("m_sessionSaveTimer");
    const auto savePos = body.find("saveAllSessions");
    bool stopFirst = false;
    if (stopPos != std::string::npos && savePos != std::string::npos) {
        // Look for ->stop() near the m_sessionSaveTimer mention.
        const std::size_t windowStart = stopPos;
        const std::size_t windowEnd = std::min(stopPos + 80, body.size());
        const std::string near = body.substr(windowStart, windowEnd - windowStart);
        if (contains(near, "stop") && stopPos < savePos) stopFirst = true;
    }
    expect(stopFirst,
           "ANTS-1159-INV-7: closeEvent stops m_sessionSaveTimer before saveAllSessions");
}

void testInv8_closeEventStillSaves() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-8: read mainwindow.cpp"); return; }

    const std::string body = extractBody(cp, "void MainWindow::closeEvent");
    expect(contains(body, "saveAllSessions"),
           "ANTS-1159-INV-8: closeEvent still calls saveAllSessions (no regression)");
}

void testInv9_saveTabOrderOnlyShortCircuitGuards() {
    const std::string cp = readFile("src/mainwindow.cpp");
    if (cp.empty()) { expect(false, "ANTS-1159-INV-9: read mainwindow.cpp"); return; }

    const std::string body = extractBody(cp, "void MainWindow::saveTabOrderOnly");
    expect(!body.empty(), "ANTS-1159-INV-9: saveTabOrderOnly body extracted");

    // Both guards must appear: sessionPersistence() check AND
    // m_uptimeTimer.elapsed() < 5000 check.
    expect(contains(body, "sessionPersistence"),
           "ANTS-1159-INV-9: saveTabOrderOnly guards on sessionPersistence()");
    expect(contains(body, "m_uptimeTimer") && contains(body, "5000"),
           "ANTS-1159-INV-9: saveTabOrderOnly guards on 5 s uptime floor");
}

}  // namespace

int main(int /*argc*/, char * /*argv*/[]) {
    testInv1_timerMemberDeclaredAndInitialised();
    testInv2_timerConnectedToSaveAllSessions();
    testInv3_saveTabOrderOnlyDeclaredAndDefined();
    testInv4_calledFromNewTab();
    testInv5_calledFromPerformTabClose();
    testInv6_connectedToTabMoved();
    testInv7_closeEventStopsTimer();
    testInv8_closeEventStillSaves();
    testInv9_saveTabOrderOnlyShortCircuitGuards();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nAll INVs PASS.\n");
    return 0;
}
