// Feature-conformance test for ANTS-1147 (0.7.74) —
// themedstylesheet extracted from mainwindow.cpp into
// src/themedstylesheet.{cpp,h}. Hybrid harness: source-grep for
// INVs 1-7, unit-level helper tests for INV-8.
//
// Links against src/themedstylesheet.cpp + src/themes.cpp + Qt6::Core
// + Qt6::Gui (for QColor) so INV-8 can call the pure helpers
// directly without instantiating any QWidget.

#include <cstdio>
#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
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


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

bool containsQ(const QString &hay, const char *needle) {
    return hay.contains(QString::fromUtf8(needle));
}

void fail(const char *label, const std::string &why) {
    ADD_FAILURE() << "[" << label << "] " << why;
}

std::size_t lineCount(const std::string &text) {
    std::size_t n = 0;
    for (char c : text) if (c == '\n') ++n;
    return n + (text.empty() ? 0 : 1);
}

}  // namespace

TEST(ThemedstylesheetExtraction, Main) {

    const std::string mw     = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string mwH    = ants_test::slurpFile(SRC_MAINWINDOW_H_PATH);
    const std::string tssCpp = ants_test::slurpFile(SRC_THEMEDSTYLESHEET_CPP_PATH);
    const std::string tssH   = ants_test::slurpFile(SRC_THEMEDSTYLESHEET_H_PATH);

    if (mw.empty())  { fail("setup", "mainwindow.cpp not readable"); return; }
    if (mwH.empty()) { fail("setup", "mainwindow.h not readable"); return; }

    // INV-1 — six public-helper signatures byte-for-byte.
    if (tssH.empty())
        { fail("INV-1", "src/themedstylesheet.h not present — extraction not done"); return; }
    if (!contains(tssH, "namespace themedstylesheet"))
        { fail("INV-1", "themedstylesheet.h missing `namespace themedstylesheet`"); return; }
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
            { fail("INV-1", std::string("themedstylesheet.h missing signature `") + sig + "`"); return; }
    }

    // INV-2 — migrated QSS selectors byte-for-byte in the new TU.
    // (QProgressBar::chunk excluded — non-distinctive vs claudestatuswidgets.cpp.)
    if (tssCpp.empty())
        { fail("INV-2", "src/themedstylesheet.cpp not present — extraction not done"); return; }
    // NB: the QTabBar::close-button data-URI SVG markers were removed in
    // ANTS-2098 — that QSS rule never rendered (Qt6 QSS can't load a
    // `data:` image) and was deleted; the close glyph is now a themed
    // QToolButton drawn by ColoredTabBar, not a stylesheet image.
    static const char *kQssMarkers[] = {
        "QMainWindow {",
        "QMenuBar::item:hover",
        "QMenuBar::item:selected",
        "QTabBar::tab:selected",
        "QPushButton:hover:enabled",
        "QDialog {",
        "QListWidget#commandPaletteList",
        "QScrollBar:vertical",
        "QToolTip {",
        "border-radius: 3px",
        "font-weight: 600",
    };
    for (const char *m : kQssMarkers) {
        if (!contains(tssCpp, m))
            { fail("INV-2", std::string("themedstylesheet.cpp missing QSS marker `") + m + "`"); return; }
    }

    // INV-3 — distinctive QSS markers no longer present in mainwindow.cpp.
    static const char *kRemovedFromMw[] = {
        "data:image/svg+xml;utf8",
        "QListWidget#commandPaletteList::item:selected",
        "QPushButton:hover:enabled",
    };
    for (const char *m : kRemovedFromMw) {
        if (contains(mw, m))
            { fail("INV-3", std::string("mainwindow.cpp still contains migrated QSS marker `") + m + "`"); return; }
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
            { fail("INV-4", std::string("mainwindow.cpp missing call to `") + c + "`"); return; }
    }

    // INV-5 — refreshRepoVisibility reuses buildChipStylesheet. The
    // function definition + a buildChipStylesheet call inside it.
    {
        const auto sigPos = mw.find("void MainWindow::refreshRepoVisibility(");
        if (sigPos == std::string::npos)
            { fail("INV-5", "mainwindow.cpp missing MainWindow::refreshRepoVisibility"); return; }
        const auto nextDef = mw.find("\nvoid MainWindow::", sigPos + 1);
        const std::string body = mw.substr(sigPos,
            nextDef == std::string::npos ? std::string::npos : nextDef - sigPos);
        if (!contains(body, "themedstylesheet::buildChipStylesheet("))
            { fail("INV-5", "refreshRepoVisibility no longer reuses buildChipStylesheet"); return; }
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
            { fail("INV-6", std::string("mainwindow.h missing cache member `") + m + "`"); return; }
    }
    {
        const auto sigPos = mw.find("void MainWindow::updateStatusBar(");
        if (sigPos == std::string::npos)
            { fail("INV-6", "mainwindow.cpp missing MainWindow::updateStatusBar"); return; }
        const auto nextDef = mw.find("\nvoid MainWindow::", sigPos + 1);
        const std::string body = mw.substr(sigPos,
            nextDef == std::string::npos ? std::string::npos : nextDef - sigPos);
        if (!contains(body, "themedstylesheet::buildChipStylesheet("))
            { fail("INV-6", "updateStatusBar does not call themedstylesheet::buildChipStylesheet"); return; }
        if (!contains(body, "newQss != m_lastBranchChipQss"))
            { fail("INV-6", "updateStatusBar missing string-comparison guard `newQss != m_lastBranchChipQss`"); return; }
        if (!contains(body, "m_lastBranchChipValid = true"))
            { fail("INV-6", "updateStatusBar missing `m_lastBranchChipValid = true` post-update"); return; }
    }

    // INV-7 — two-sided LoC anchor.
    const std::size_t tssLoc = lineCount(tssCpp);
    if (tssLoc < 200)
        { fail("INV-7",
            "themedstylesheet.cpp has only " + std::to_string(tssLoc) +
            " lines; sanity floor is 200"); return; }

    // INV-8 — unit-level helper tests. Direct calls into the
    // pure functions; assert structural substrings.
    // Copy, not a reference: Themes::byName returns a ref into the stable
    // static table (non-dangling), but GCC's -Wdangling-reference heuristic
    // false-positives on `const T& = func_returning_ref(arg)`. Theme is a
    // small value struct, so a copy is free here and keeps the build clean.
    const Theme theme = Themes::byName(QStringLiteral("dark"));
    {
        const QString app = themedstylesheet::buildAppStylesheet(theme);
        if (!containsQ(app, "QMainWindow { background-color:"))
            { fail("INV-8", "buildAppStylesheet missing QMainWindow rule"); return; }
        if (!containsQ(app, "QPushButton:hover:enabled"))
            { fail("INV-8", "buildAppStylesheet missing QPushButton:hover:enabled rule"); return; }
        // (No tab-close data-URI assertion — that QSS rule was removed in
        // ANTS-2098; it never rendered, the close glyph is a themed
        // QToolButton drawn by ColoredTabBar now.)
    }
    {
        const QString chip4 =
            themedstylesheet::buildChipStylesheet(theme, QColor("#00ff00"), 4);
        if (!containsQ(chip4, "color: #00ff00"))
            { fail("INV-8", "buildChipStylesheet didn't render fgColor `#00ff00`"); return; }
        if (!containsQ(chip4, "margin: 2px 6px 2px 4px"))
            { fail("INV-8", "buildChipStylesheet didn't render leftMarginPx=4"); return; }
        if (!containsQ(chip4, "border-radius: 3px"))
            { fail("INV-8", "buildChipStylesheet missing border-radius"); return; }
        if (!containsQ(chip4, "font-weight: 600"))
            { fail("INV-8", "buildChipStylesheet missing font-weight: 600"); return; }
    }
    {
        const QString chip0 =
            themedstylesheet::buildChipStylesheet(theme, QColor("#ff0000"), 0);
        if (!containsQ(chip0, "margin: 2px 6px 2px 0"))
            { fail("INV-8", "buildChipStylesheet didn't render leftMarginPx=0 (visibility-badge variant)"); return; }
    }

    std::fprintf(stderr,
        "OK — themedstylesheet extraction INVs hold "
        "(themedstylesheet.cpp = %zu LoC).\n",
        tssLoc);
}
