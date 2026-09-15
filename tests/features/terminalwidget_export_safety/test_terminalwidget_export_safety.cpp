// ANTS-5078 — feature-conformance test; see spec.md. Source scrapes: the
// exports run from a context menu and a file dialog on a live TerminalWidget,
// which the unit harness cannot drive.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

namespace {

const std::size_t npos = std::string::npos;

std::string between(const std::string &s, const std::string &from,
                    const std::string &to) {
    const std::size_t a = s.find(from);
    if (a == npos) return {};
    const std::size_t b = s.find(to, a + from.size());
    if (b == npos) return {};
    return s.substr(a, b - a);
}

std::string contextMenu() {
    return ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALWIDGET_PATH, "void TerminalWidget::contextMenuEvent("));
}

}  // namespace

// INV-1
TEST(TerminalWidgetExportSafety, Inv1TextExportReportsFailure) {
    const std::string h = between(contextMenu(),
                                  "\"Export Scrollback as Text...\"", "QAction *");
    ASSERT_FALSE(h.empty()) << "setup: the text export handler was not found";
    EXPECT_NE(h.find("QSaveFile file(path)"), npos)
        << "INV-1: text export does not write through QSaveFile";
    EXPECT_NE(h.find("file.commit()"), npos)
        << "INV-1: text export does not check commit()";
    EXPECT_NE(h.find("emit captureFailed("), npos)
        << "INV-1: a failed text export is not reported";
}

// INV-2
TEST(TerminalWidgetExportSafety, Inv2HtmlExportReportsFailure) {
    const std::string h = between(contextMenu(),
                                  "\"Export Scrollback as HTML...\"", "menu.exec(");
    ASSERT_FALSE(h.empty()) << "setup: the HTML export handler was not found";
    EXPECT_NE(h.find("QSaveFile file(path)"), npos)
        << "INV-2: HTML export does not write through QSaveFile";
    EXPECT_NE(h.find("file.commit()"), npos)
        << "INV-2: HTML export does not check commit()";
    EXPECT_NE(h.find("emit captureFailed("), npos)
        << "INV-2: a failed HTML export is not reported";
}

// INV-3
TEST(TerminalWidgetExportSafety, Inv3BlockCastChecksItsWrites) {
    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALWIDGET_PATH, "bool TerminalWidget::exportBlockAsCast("));
    ASSERT_FALSE(body.empty()) << "setup: exportBlockAsCast was not found";
    EXPECT_NE(body.find("QSaveFile file(path)"), npos)
        << "INV-3: exportBlockAsCast does not write through QSaveFile";
    EXPECT_EQ(body.find("QFile file(path)"), npos)
        << "INV-3: exportBlockAsCast still truncates the target with QFile";
    EXPECT_NE(body.find("file.commit()"), npos)
        << "INV-3: exportBlockAsCast does not check commit()";
}

// INV-4
TEST(TerminalWidgetExportSafety, Inv4ShareBlockReportsFailure) {
    const std::string h = between(contextMenu(),
                                  "\"Share Block as .cast...\"", "menu.addSeparator()");
    ASSERT_FALSE(h.empty()) << "setup: the Share Block handler was not found";
    EXPECT_NE(h.find("exportBlockAsCast(idx, path)"), npos)
        << "setup: Share Block no longer calls exportBlockAsCast";
    EXPECT_NE(h.find("emit captureFailed("), npos)
        << "INV-4: a failed Share Block export is not reported";
}
