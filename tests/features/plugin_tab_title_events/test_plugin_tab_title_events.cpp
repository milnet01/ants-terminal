// Plugins hear title changes, new tabs and closed tabs — see spec.md.
// ANTS-4272. Source-scrape of the MainWindow sources.

#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

namespace {

std::string body(const char *signature) {
    return ants_test::slurpFunctionBody(ants_test::slurpMainWindow(),
                                        signature);
}

bool has(const std::string &text, const char *needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

// INV-1
TEST(PluginTabTitleEvents, TitleChangeFires) {
    const std::string b = body("void MainWindow::connectTerminal(");
    ASSERT_FALSE(b.empty()) << "connectTerminal not found";
    EXPECT_TRUE(has(b, "fireEvent(PluginEvent::TitleChanged, title)"));
}

// INV-2
TEST(PluginTabTitleEvents, NewTabsAndPanesFireCreated) {
    for (const char *sig : {"void MainWindow::newTab()",
                            "int MainWindow::newTabForRemote(",
                            "void MainWindow::splitCurrentPane(",
                            "void MainWindow::restoreSessions("}) {
        const std::string b = body(sig);
        ASSERT_FALSE(b.empty()) << sig << " not found";
        EXPECT_TRUE(has(b, "firePluginTabEvent(true,")) << sig;
    }
    // A restored tab's index is read after the default tab is removed.
    const std::string r = body("void MainWindow::restoreSessions(");
    const std::size_t removed = r.find("m_tabWidget->removeTab(0)");
    const std::size_t fired = r.find("firePluginTabEvent(true,");
    ASSERT_NE(removed, std::string::npos);
    ASSERT_NE(fired, std::string::npos);
    EXPECT_LT(removed, fired);
}

// INV-3
TEST(PluginTabTitleEvents, ClosedTabsAndPanesFireClosed) {
    for (const char *sig : {"void MainWindow::performTabClose(",
                            "void MainWindow::closeFocusedPane(",
                            "void MainWindow::connectTerminal("}) {
        const std::string b = body(sig);
        ASSERT_FALSE(b.empty()) << sig << " not found";
        EXPECT_TRUE(has(b, "firePluginTabEvent(false,")) << sig;
    }
}

// INV-4
TEST(PluginTabTitleEvents, TabEventPayload) {
    const std::string b = body("void MainWindow::firePluginTabEvent(");
    ASSERT_FALSE(b.empty()) << "firePluginTabEvent not found";
    EXPECT_TRUE(has(b, "PluginEvent::TabCreated"));
    EXPECT_TRUE(has(b, "PluginEvent::TabClosed"));
    EXPECT_TRUE(has(b, "\"index=%1&split=%2\""));
}
