// ANTS-5034 — feature-conformance test; see spec.md. Source scrape: a
// MainWindow cannot be built headless in this bundle.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

// INV-1 — the opacity action applies the level to every live terminal.
TEST(ViewOpacityLive, Inv1ActionUpdatesLiveTerminals) {
    const std::string src =
        ants_test::stripComments(ants_test::slurpFile(SRC_MAINWINDOW_PATH));
    const std::string body =
        ants_test::slurpFunctionBody(src, "void MainWindow::setupViewMenu(");
    ASSERT_FALSE(body.empty()) << "setupViewMenu not found";

    // The handler runs from the saved level to the end of its lambda.
    const std::string handler =
        ants_test::regionBetween(body, "m_config.setOpacity(val);", "});");
    ASSERT_FALSE(handler.empty()) << "the opacity action's handler was not found";
    EXPECT_NE(handler.find("liveTerminals()"), std::string::npos)
        << "the handler must walk the open terminals";
    EXPECT_NE(handler.find("setWindowOpacityLevel(val)"), std::string::npos)
        << "the handler must apply the chosen level to each of them";
}
