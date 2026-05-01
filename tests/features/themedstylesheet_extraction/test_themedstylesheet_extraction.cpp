// Feature-conformance test for ANTS-1147 (0.7.74) —
// themedstylesheet extracted from mainwindow.cpp into
// src/themedstylesheet.{cpp,h}. Hybrid harness: source-grep for
// INVs 1-7, unit-level helper tests for INV-8.
//
// Links against src/themedstylesheet.cpp + src/themes.cpp + Qt6::Core
// + Qt6::Gui (for QColor) so INV-8 can call the pure helpers
// directly without instantiating any QWidget.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <QColor>
#include <QString>

#include "themedstylesheet.h"
#include "themes.h"

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_H_PATH
#error "SRC_MAINWINDOW_H_PATH compile definition required"
#endif
#ifndef SRC_THEMEDSTYLESHEET_CPP_PATH
#error "SRC_THEMEDSTYLESHEET_CPP_PATH compile definition required"
#endif
#ifndef SRC_THEMEDSTYLESHEET_H_PATH
#error "SRC_THEMEDSTYLESHEET_H_PATH compile definition required"
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

bool containsQ(const QString &hay, const char *needle) {
    return hay.contains(QString::fromUtf8(needle));
}

int fail(const char *label, const std::string &why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why.c_str());
    return 1;
}

std::size_t lineCount(const std::string &text) {
    std::size_t n = 0;
    for (char c : text) if (c == '\n') ++n;
    return n + (text.empty() ? 0 : 1);
}

}  // namespace

int main() {
    const std::string mw     = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string mwH    = slurp(SRC_MAINWINDOW_H_PATH);
    const std::string tssCpp = slurp(SRC_THEMEDSTYLESHEET_CPP_PATH);
    const std::string tssH   = slurp(SRC_THEMEDSTYLESHEET_H_PATH);

    if (mw.empty())  return fail("setup", "mainwindow.cpp not readable");
    if (mwH.empty()) return fail("setup", "mainwindow.h not readable");

    // INV-1 — six public-helper signatures byte-for-byte.
    if (tssH.empty())
        return fail("INV-1", "src/themedstylesheet.h not present — extraction not done");
    if (!contains(tssH, "namespace themedstylesheet"))
        return fail("INV-1", "themedstylesheet.h missing `namespace themedstylesheet`");
    static const char *kSignatures[] = {
        "QString buildAppStylesheet(const Theme &theme);",
        "QString buildMenuBarStylesheet(const Theme &theme);",
        "QString buildStatusMessageStylesheet(const Theme &theme);",
        "QString buildStatusProcessStylesheet(const Theme &theme);",
        "QString buildGitSeparatorStylesheet(const Theme &theme);",
        "QString buildChipStylesheet(const Theme &theme, const QColor &fgColor, int leftMarginPx);",
    };
    for (const char *sig : kSignatures) {
        if (!contains(tssH, sig))
            return fail("INV-1", std::string("themedstylesheet.h missing signature `") + sig + "`");
    }

    // INV-2 — migrated QSS selectors byte-for-byte in the new TU.
    // (QProgressBar::chunk excluded — non-distinctive vs claudestatuswidgets.cpp.)
    if (tssCpp.empty())
        return fail("INV-2", "src/themedstylesheet.cpp not present — extraction not done");
    static const char *kQssMarkers[] = {
        "QMainWindow {",
        "QMenuBar::item:hover",
        "QMenuBar::item:selected",
        "QTabBar::tab:selected",
        "QTabBar::close-button {",
        "QPushButton:hover:enabled",
        "QDialog {",
        "QListWidget#commandPaletteList",
        "QScrollBar:vertical",
        "QToolTip {",
        "data:image/svg+xml;utf8",
        "viewBox='0 0 10 10'",
        "stroke-linecap='round'",
        "border-radius: 3px",
        "font-weight: 600",
    };
    for (const char *m : kQssMarkers) {
        if (!contains(tssCpp, m))
            return fail("INV-2", std::string("themedstylesheet.cpp missing QSS marker `") + m + "`");
    }

    // INV-3 — distinctive QSS markers no longer present in mainwindow.cpp.
    static const char *kRemovedFromMw[] = {
        "data:image/svg+xml;utf8",
        "QListWidget#commandPaletteList::item:selected",
        "QPushButton:hover:enabled",
    };
    for (const char *m : kRemovedFromMw) {
        if (contains(mw, m))
            return fail("INV-3", std::string("mainwindow.cpp still contains migrated QSS marker `") + m + "`");
    }

    // INV-4 — applyTheme calls each of the six helpers.
    static const char *kHelperCalls[] = {
        "themedstylesheet::buildAppStylesheet(",
        "themedstylesheet::buildMenuBarStylesheet(",
        "themedstylesheet::buildStatusMessageStylesheet(",
        "themedstylesheet::buildStatusProcessStylesheet(",
        "themedstylesheet::buildGitSeparatorStylesheet(",
        "themedstylesheet::buildChipStylesheet(",
    };
    for (const char *c : kHelperCalls) {
        if (!contains(mw, c))
            return fail("INV-4", std::string("mainwindow.cpp missing call to `") + c + "`");
    }

    // INV-5 — refreshRepoVisibility reuses buildChipStylesheet. The
    // function definition + a buildChipStylesheet call inside it.
    {
        const auto sigPos = mw.find("void MainWindow::refreshRepoVisibility(");
        if (sigPos == std::string::npos)
            return fail("INV-5", "mainwindow.cpp missing MainWindow::refreshRepoVisibility");
        const auto nextDef = mw.find("\nvoid MainWindow::", sigPos + 1);
        const std::string body = mw.substr(sigPos,
            nextDef == std::string::npos ? std::string::npos : nextDef - sigPos);
        if (!contains(body, "themedstylesheet::buildChipStylesheet("))
            return fail("INV-5", "refreshRepoVisibility no longer reuses buildChipStylesheet");
    }

    // INV-6 — cache-and-compare guard locked in.
    // ANTS-1147 debt-sweep: m_lastBranchChipPrimary + m_lastBranchChipTheme
    // were write-only when the spec landed; deleted in the post-1147 sweep
    // since the QSS string already encodes (theme × primary × margin)
    // and the cache compare uses only the string. INV-6 now asserts the
    // two surviving members.
    static const char *kCacheMembers[] = {
        "m_lastBranchChipValid",
        "m_lastBranchChipQss",
    };
    for (const char *m : kCacheMembers) {
        if (!contains(mwH, m))
            return fail("INV-6", std::string("mainwindow.h missing cache member `") + m + "`");
    }
    {
        const auto sigPos = mw.find("void MainWindow::updateStatusBar(");
        if (sigPos == std::string::npos)
            return fail("INV-6", "mainwindow.cpp missing MainWindow::updateStatusBar");
        const auto nextDef = mw.find("\nvoid MainWindow::", sigPos + 1);
        const std::string body = mw.substr(sigPos,
            nextDef == std::string::npos ? std::string::npos : nextDef - sigPos);
        if (!contains(body, "themedstylesheet::buildChipStylesheet("))
            return fail("INV-6", "updateStatusBar does not call themedstylesheet::buildChipStylesheet");
        if (!contains(body, "newQss != m_lastBranchChipQss"))
            return fail("INV-6", "updateStatusBar missing string-comparison guard `newQss != m_lastBranchChipQss`");
        if (!contains(body, "m_lastBranchChipValid = true"))
            return fail("INV-6", "updateStatusBar missing `m_lastBranchChipValid = true` post-update");
    }

    // INV-7 — two-sided LoC anchor.
    const std::size_t tssLoc = lineCount(tssCpp);
    if (tssLoc < 200)
        return fail("INV-7",
            "themedstylesheet.cpp has only " + std::to_string(tssLoc) +
            " lines; sanity floor is 200");

    // INV-8 — unit-level helper tests. Direct calls into the
    // pure functions; assert structural substrings.
    const Theme &theme = Themes::byName(QStringLiteral("dark"));
    {
        const QString app = themedstylesheet::buildAppStylesheet(theme);
        if (!containsQ(app, "QMainWindow { background-color:"))
            return fail("INV-8", "buildAppStylesheet missing QMainWindow rule");
        if (!containsQ(app, "QPushButton:hover:enabled"))
            return fail("INV-8", "buildAppStylesheet missing QPushButton:hover:enabled rule");
        if (!containsQ(app, "data:image/svg+xml;utf8"))
            return fail("INV-8", "buildAppStylesheet missing tab-close SVG data URI");
    }
    {
        const QString chip4 =
            themedstylesheet::buildChipStylesheet(theme, QColor("#00ff00"), 4);
        if (!containsQ(chip4, "color: #00ff00"))
            return fail("INV-8", "buildChipStylesheet didn't render fgColor `#00ff00`");
        if (!containsQ(chip4, "margin: 2px 6px 2px 4px"))
            return fail("INV-8", "buildChipStylesheet didn't render leftMarginPx=4");
        if (!containsQ(chip4, "border-radius: 3px"))
            return fail("INV-8", "buildChipStylesheet missing border-radius");
        if (!containsQ(chip4, "font-weight: 600"))
            return fail("INV-8", "buildChipStylesheet missing font-weight: 600");
    }
    {
        const QString chip0 =
            themedstylesheet::buildChipStylesheet(theme, QColor("#ff0000"), 0);
        if (!containsQ(chip0, "margin: 2px 6px 2px 0"))
            return fail("INV-8", "buildChipStylesheet didn't render leftMarginPx=0 (visibility-badge variant)");
    }

    std::fprintf(stderr,
        "OK — themedstylesheet extraction INVs hold "
        "(themedstylesheet.cpp = %zu LoC).\n",
        tssLoc);
    return 0;
}
