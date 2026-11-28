// The window's state follows what changed — see spec.md. ANTS-6636.
// Source-scrape of the MainWindow sources, comments stripped.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {

std::string source() {
    return ants_test::stripComments(ants_test::slurpMainWindow());
}

std::string body(const std::string &signature) {
    return ants_test::slurpFunctionBody(source(), signature);
}

bool has(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1
TEST(MainwindowStateSync, PromotedPaneKeepsTheCurrentTab) {
    const std::string b = body("void MainWindow::cleanupEmptySplitters(");
    ASSERT_FALSE(b.empty()) << "cleanupEmptySplitters not found";
    const std::size_t saved = b.find("m_tabWidget->currentWidget()");
    const std::size_t removed = b.find("m_tabWidget->removeTab(tabIdx)");
    const std::size_t restored = b.find("m_tabWidget->setCurrentWidget(");
    ASSERT_NE(removed, std::string::npos);
    ASSERT_NE(saved, std::string::npos)
        << "INV-1: the current tab is not recorded before the promotion";
    ASSERT_NE(restored, std::string::npos)
        << "INV-1: the current tab is not restored after the promotion";
    EXPECT_LT(saved, removed);
    EXPECT_LT(removed, restored);
    EXPECT_FALSE(has(b, "setCurrentIndex(tabIdx)"))
        << "INV-1: a shell exiting in a background tab switches the user to it";
}

// INV-2
TEST(MainwindowStateSync, ThemeReloadRebuildsThePalette) {
    const std::string w = ants_test::regionBetween(
        source(), "\"Reload &User Themes\"", "\"Themes reloaded\"");
    ASSERT_FALSE(w.empty()) << "Reload User Themes handler not found";
    const std::size_t added = w.find("addThemeActions(themesMenu)");
    const std::size_t rebuilt = w.find("rebuildCommandPalette()");
    ASSERT_NE(added, std::string::npos);
    ASSERT_NE(rebuilt, std::string::npos)
        << "INV-2: the palette keeps the old theme proxies after a reload";
    EXPECT_LT(added, rebuilt);
}

// INV-3
TEST(MainwindowStateSync, AutoProfileCacheGenerationIsPerWindow) {
    const std::string b = body("void MainWindow::checkAutoProfileRules(");
    ASSERT_FALSE(b.empty()) << "checkAutoProfileRules not found";
    EXPECT_TRUE(has(b, "m_autoProfileRulesGenSeen"))
        << "INV-3: the last-seen rules generation is not per window";
    EXPECT_FALSE(has(b, "static quint64"))
        << "INV-3: a shared last-seen generation makes two windows clear the "
           "caches on every tick";
}

// INV-4
TEST(MainwindowStateSync, ColourSchemeSignalAlwaysConnected) {
    const std::string ctor = body("MainWindow::MainWindow(bool quakeMode");
    ASSERT_FALSE(ctor.empty()) << "MainWindow constructor not found";
    EXPECT_TRUE(has(ctor, "&QStyleHints::colorSchemeChanged"));
    EXPECT_FALSE(has(ctor, "autoColorScheme()"))
        << "INV-4: the signal is wired only when the setting was on at start";
    const std::string slot = body("void MainWindow::onSystemColorSchemeChanged(");
    EXPECT_TRUE(has(slot, "if (!m_config.autoColorScheme()) return;"))
        << "INV-4: the slot must check the setting itself";
}
