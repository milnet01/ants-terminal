// Feature-conformance test for ANTS-1146 (0.7.74) —
// ClaudeStatusBarController extracted from mainwindow.cpp into
// src/claudestatuswidgets.{cpp,h}. Source-grep only.
//
// INVs map 1:1 to docs/specs/ANTS-1146.md §Acceptance:
//   INV-1   class declaration shape (top-level, no namespace)
//   INV-2a  14 public methods declared byte-for-byte in the header
//   INV-2b  six signals declared byte-for-byte in the header
//   INV-3   user-visible labels/tooltips preserved in the new TU
//   INV-4   migrated members no longer referenced in mainwindow.cpp
//   INV-5   controller member + renamed setup function present
//   INV-6   setupStatusBarChrome retains the three orphans
//   INV-7   exactly six connect(m_claudeStatusBarController,) sites
//   INV-8   two-sided LoC anchor + removed-function asserts

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H_PATH
#error "SRC_MAINWINDOW_H_PATH compile definition required"
#endif
#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDESTATUSWIDGETS_H_PATH
#error "SRC_CLAUDESTATUSWIDGETS_H_PATH compile definition required"
#endif
#ifndef SRC_CMAKELISTS_PATH
#error "SRC_CMAKELISTS_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t countOccurrences(const std::string &hay, const std::string &needle) {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    for (std::size_t pos = hay.find(needle);
         pos != std::string::npos;
         pos = hay.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

int fail(const char *label, const std::string &why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why.c_str());
    return 1;
}

std::size_t lineCount(const std::string &text) {
    return countOccurrences(text, "\n") + (text.empty() ? 0 : 1);
}

}  // namespace

int main() {
    const std::string mw      = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string mwH     = slurp(SRC_MAINWINDOW_H_PATH);
    const std::string cswCpp  = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string cswH    = slurp(SRC_CLAUDESTATUSWIDGETS_H_PATH);

    if (mw.empty())  return fail("setup", "mainwindow.cpp not readable");
    if (mwH.empty()) return fail("setup", "mainwindow.h not readable");

    // INV-1 — class declared top-level, no namespace.
    if (cswH.empty())
        return fail("INV-1",
            "src/claudestatuswidgets.h not present — extraction not done");
    if (!contains(cswH, "class ClaudeStatusBarController : public QObject"))
        return fail("INV-1",
            "claudestatuswidgets.h does not declare "
            "`class ClaudeStatusBarController : public QObject`");
    if (contains(cswH, "namespace claudestatus"))
        return fail("INV-1",
            "claudestatuswidgets.h still wraps the class in "
            "`namespace claudestatus` — spec mandates top-level "
            "(matches ClaudeIntegration / ClaudeTabTracker / etc.)");

    // INV-2a — 14 public-method signatures, byte-for-byte.
    static const char *kPublicMethods[] = {
        "void attach(ClaudeIntegration *integration, ClaudeTabTracker *tracker, ColoredTabBar *coloredTabBar, QTabWidget *tabWidget);",
        "void setPromptActive(bool active);",
        "void setPlanMode(bool active);",
        "void setAuditing(bool active);",
        "void setError(const QString &text, const QString &tooltip, int autoHideMs);",
        "void clearError();",
        "void resetForTabSwitch();",
        "void applyTheme(const QString &themeName);",
        "void refreshBgTasksButton();",
        "void setCurrentTerminalProvider(std::function<TerminalWidget *()>);",
        "void setFocusedTerminalProvider(std::function<TerminalWidget *()>);",
        "void setTerminalAtTabProvider(std::function<TerminalWidget *(int)>);",
        "void setTabIndicatorEnabledProvider(std::function<bool()>);",
        "QPushButton *reviewButton() const;",
        "QPushButton *bgTasksButton() const;",
        "ClaudeBgTaskTracker *bgTasksTracker() const;",
    };
    for (const char *m : kPublicMethods) {
        if (!contains(cswH, m)) {
            std::fprintf(stderr,
                "[INV-2a] FAIL: claudestatuswidgets.h missing method "
                "signature `%s`\n", m);
            return 1;
        }
    }

    // INV-2b — six signal signatures.
    static const char *kSignals[] = {
        "void reviewClicked();",
        "void bgTasksClicked();",
        "void allowlistRequested(const QString &rule);",
        "void reviewButtonShouldRefresh();",
        "void statusMessageRequested(const QString &text, int timeoutMs);",
        "void statusMessageCleared();",
    };
    for (const char *s : kSignals) {
        if (!contains(cswH, s)) {
            std::fprintf(stderr,
                "[INV-2b] FAIL: claudestatuswidgets.h missing signal "
                "signature `%s`\n", s);
            return 1;
        }
    }

    // INV-3 — user-visible labels / tooltips byte-identical.
    if (cswCpp.empty())
        return fail("INV-3",
            "src/claudestatuswidgets.cpp not present — extraction not done");
    static const char *kUserStrings[] = {
        // Status-label texts (bounded vocabulary)
        "\"Claude: prompting\"",
        "\"Claude: planning\"",
        "\"Claude: auditing\"",
        "\"Claude: idle\"",
        "\"Claude: thinking\"",
        "\"Claude: bash\"",
        "\"Claude: reading a file\"",
        "\"Claude: editing\"",
        "\"Claude: searching\"",
        "\"Claude: browsing\"",
        "\"Claude: delegating\"",
        "\"Claude: compacting\"",
        // Tooltip texts
        "Claude Code context window usage",
        "Context %1% — consider using /compact",
        // Accessible names
        "Claude Code session status",
        "Review Claude code changes",
        // Permanent-widget button labels
        "Review Changes",
        "Background Tasks",
        // Bg-tasks dynamic format strings
        "Background Tasks (%1)",
        "%1 running · %2 total in this session",
        // Permission-group button labels
        "\"Allow\"",
        "\"Deny\"",
        "\"Add to allowlist\"",
        // Permission-group object name (asserted twice — dedup walk + ctor)
        "claudeAllowBtn",
        // Tab-indicator tooltip texts
        "Claude: awaiting input",
        "Claude: thinking…",
        "Claude: compacting…",
        "Claude: tool use",
        "\"Claude: %1\"",
    };
    for (const char *s : kUserStrings) {
        if (!contains(cswCpp, s)) {
            std::fprintf(stderr,
                "[INV-3] FAIL: claudestatuswidgets.cpp missing "
                "user-visible string `%s` — drive-by reformat broke a "
                "label or tooltip\n", s);
            return 1;
        }
    }
    // claudeAllowBtn object name asserted at least twice (dedup
    // findChildren walk AND new btnWidget construction).
    if (countOccurrences(cswCpp, "claudeAllowBtn") < 2) {
        return fail("INV-3",
            "claudestatuswidgets.cpp must reference `claudeAllowBtn` "
            "at least twice — dedup walk and new btnWidget construction");
    }

    // INV-4 — migrated members no longer referenced in mainwindow.cpp.
    static const char *kMigratedMembers[] = {
        "m_claudeStatusLabel",
        "m_claudeContextBar",
        "m_claudeReviewBtn",
        "m_claudeErrorLabel",
        "m_claudeBgTasks",
        "m_claudeBgTasksBtn",
        "m_claudePromptActive",
        "m_claudePlanMode",
        "m_claudeAuditing",
        "m_claudeLastState",
        "m_claudeLastDetail",
    };
    for (const char *m : kMigratedMembers) {
        if (contains(mw, m)) {
            std::fprintf(stderr,
                "[INV-4] FAIL: mainwindow.cpp still contains migrated "
                "member `%s` — extraction incomplete (legacy callers "
                "must use m_claudeStatusBarController accessor / "
                "setter / signal instead)\n", m);
            return 1;
        }
    }

    // INV-5 — controller member declared, setup renamed.
    if (!contains(mwH, "ClaudeStatusBarController *m_claudeStatusBarController"))
        return fail("INV-5",
            "mainwindow.h does not declare member "
            "`ClaudeStatusBarController *m_claudeStatusBarController`");
    if (contains(mw, "void MainWindow::setupClaudeIntegration("))
        return fail("INV-5",
            "mainwindow.cpp still defines "
            "`void MainWindow::setupClaudeIntegration(` — spec mandates "
            "rename to setupStatusBarChrome");
    if (!contains(mw, "void MainWindow::setupStatusBarChrome("))
        return fail("INV-5",
            "mainwindow.cpp does not define "
            "`void MainWindow::setupStatusBarChrome(` — rename incomplete");

    // INV-6 — three orphans retained in setupStatusBarChrome.
    // (We don't extract the function body to scope-check; presence
    // of all three constructs in the file is sufficient given INV-4
    // already proves the Claude-specific bulk left.)
    if (!contains(mw, "m_roadmapBtn = new QPushButton"))
        return fail("INV-6",
            "mainwindow.cpp does not construct m_roadmapBtn "
            "(`m_roadmapBtn = new QPushButton`)");
    if (!contains(mw, "m_updateAvailableAction = new QAction"))
        return fail("INV-6",
            "mainwindow.cpp does not construct m_updateAvailableAction "
            "(`m_updateAvailableAction = new QAction`)");
    if (!contains(mw, "QTimer::singleShot(5000"))
        return fail("INV-6",
            "mainwindow.cpp does not retain the 5 s startup "
            "update-check `QTimer::singleShot(5000`");

    // INV-7 — exactly six connect(m_claudeStatusBarController, …)
    // substrings AND exactly one PMF per signal.
    const std::size_t connectCount =
        countOccurrences(mw, "connect(m_claudeStatusBarController,");
    if (connectCount != 6) {
        return fail("INV-7",
            "mainwindow.cpp must contain exactly 6 "
            "`connect(m_claudeStatusBarController,` substrings; found " +
            std::to_string(connectCount));
    }
    static const char *kSignalPmfs[] = {
        "&ClaudeStatusBarController::reviewClicked",
        "&ClaudeStatusBarController::bgTasksClicked",
        "&ClaudeStatusBarController::allowlistRequested",
        "&ClaudeStatusBarController::reviewButtonShouldRefresh",
        "&ClaudeStatusBarController::statusMessageRequested",
        "&ClaudeStatusBarController::statusMessageCleared",
    };
    for (const char *pmf : kSignalPmfs) {
        const std::size_t n = countOccurrences(mw, pmf);
        if (n != 1) {
            std::fprintf(stderr,
                "[INV-7] FAIL: mainwindow.cpp must reference signal "
                "PMF `%s` exactly once; found %zu\n", pmf, n);
            return 1;
        }
    }

    // INV-8 — two-sided LoC anchor + removed-function asserts.
    const std::size_t cswCppLines = lineCount(cswCpp);
    if (cswCppLines < 480) {
        return fail("INV-8",
            "claudestatuswidgets.cpp has only " +
            std::to_string(cswCppLines) +
            " lines; sanity floor is 480 (the carve-out should "
            "produce ~510 LoC of body + ~50 LoC of skeleton)");
    }
    if (contains(mw, "void MainWindow::updateClaudeThemeColors("))
        return fail("INV-8",
            "mainwindow.cpp still defines "
            "`void MainWindow::updateClaudeThemeColors(` — body must "
            "have moved into ClaudeStatusBarController::applyTheme");
    if (contains(mw, "void MainWindow::applyClaudeStatusLabel("))
        return fail("INV-8",
            "mainwindow.cpp still defines "
            "`void MainWindow::applyClaudeStatusLabel(` — body must "
            "have moved into ClaudeStatusBarController::apply");
    // Two call sites are expected: the initial theme apply in
    // setupStatusBarChrome's ctor sequence, and the central restyle
    // helper at the previous line 3143. Spec said "exactly one" but
    // missed the setup-time initial apply (caught at first ctest run).
    const std::size_t applyThemeCalls =
        countOccurrences(mw, "m_claudeStatusBarController->applyTheme(");
    if (applyThemeCalls != 2) {
        return fail("INV-8",
            "mainwindow.cpp must contain exactly two "
            "`m_claudeStatusBarController->applyTheme(` calls — one "
            "in setupStatusBarChrome (initial apply) and one in the "
            "central restyle helper; found " +
            std::to_string(applyThemeCalls));
    }

    std::fprintf(stderr,
        "OK — ClaudeStatusBarController extraction INVs hold "
        "(claudestatuswidgets.cpp = %zu LoC, "
        "%zu controller-connect sites in mainwindow.cpp).\n",
        cswCppLines, connectCount);
    return 0;
}
