// Broadcast input stays in one tab, starts off, shows while on, and is
// encoded for each receiving pane — see spec.md. ANTS-5223, ANTS-5224.
// Source-scrape: MainWindow and TerminalWidget both need a live PTY.

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#ifndef SRC_TERMINALWIDGET_PATH
#define SRC_TERMINALWIDGET_PATH ""
#endif
#ifndef SRC_MAINWINDOW_H_PATH
#define SRC_MAINWINDOW_H_PATH ""
#endif

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

std::string mainWindow() {
    return ants_test::stripComments(ants_test::slurpMainWindow());
}

std::string body(const std::string &src, const std::string &anchor) {
    return ants_test::slurpFunctionBody(src, anchor);
}

}  // namespace

// INV-1
TEST(BroadcastInput, CallbackStaysInTheSourceTab) {
    const std::string cb = body(mainWindow(), "setBroadcastCallback(");
    ASSERT_FALSE(cb.empty()) << "broadcast callback not found";
    EXPECT_FALSE(contains(cb, "liveTerminals()"))
        << "the callback walks every terminal in every tab";
    EXPECT_FALSE(contains(cb, "m_allTerminals"));
    EXPECT_TRUE(contains(cb, "tabPageOf(m_tabWidget, source)"));
}

// INV-2
TEST(BroadcastInput, StartsOffOnEveryLaunch) {
    const std::string src = mainWindow();
    ASSERT_FALSE(src.empty());
    EXPECT_FALSE(contains(src, "broadcastMode()"))
        << "a MainWindow source reads the saved broadcast mode";
    EXPECT_FALSE(contains(src, "broadcast_mode"));
    const std::string header = ants_test::slurpFile(SRC_MAINWINDOW_H_PATH);
    ASSERT_FALSE(header.empty()) << "SRC_MAINWINDOW_H_PATH not readable";
    EXPECT_TRUE(contains(header, "bool m_broadcastMode = false;"));
}

// INV-3
TEST(BroadcastInput, ChipShowsWhileOn) {
    const std::string src = mainWindow();
    EXPECT_TRUE(contains(src, "addWidget(m_broadcastChip)"));
    const std::string refresh = body(src, "void MainWindow::refreshBroadcastChip(");
    ASSERT_FALSE(refresh.empty()) << "refreshBroadcastChip not found";
    EXPECT_TRUE(contains(refresh, "setVisible(m_broadcastMode)"));
    const std::string toggle =
        body(src, "connect(m_broadcastAction, &QAction::toggled");
    ASSERT_FALSE(toggle.empty()) << "Broadcast toggle handler not found";
    EXPECT_TRUE(contains(toggle, "refreshBroadcastChip()"));
    const std::string theme = body(src, "void MainWindow::applyTheme(");
    ASSERT_FALSE(theme.empty());
    EXPECT_TRUE(contains(theme, "refreshBroadcastChip()"))
        << "a theme switch leaves the chip in the old colours";
}

// INV-4 — the MainWindow half.
TEST(BroadcastInput, TargetsEncodeForTheirOwnModes) {
    const std::string cb = body(mainWindow(), "setBroadcastCallback(");
    ASSERT_FALSE(cb.empty());
    EXPECT_TRUE(contains(cb, "encodeKey(event)"));
    EXPECT_FALSE(contains(cb, "sendToPty(data)"))
        << "the source's encoded bytes are forwarded unchanged";
}

// INV-4 — the TerminalWidget half.
TEST(BroadcastInput, KeysCarryTheirEventToTheBroadcast) {
    const std::string src =
        ants_test::stripComments(ants_test::slurpFile(SRC_TERMINALWIDGET_PATH));
    ASSERT_FALSE(src.empty()) << "SRC_TERMINALWIDGET_PATH not readable";

    const std::string send = body(src, "void TerminalWidget::sendKeyData(");
    ASSERT_FALSE(send.empty());
    EXPECT_TRUE(contains(send, "m_broadcastCallback(this, event)"));

    const std::string press = body(src, "void TerminalWidget::keyPressEvent(");
    ASSERT_FALSE(press.empty());
    std::size_t calls = 0;
    for (std::size_t pos = press.find("sendKeyData("); pos != std::string::npos;
         pos = press.find("sendKeyData(", pos + 1)) {
        ++calls;
        const std::string call = press.substr(pos, press.find(';', pos) - pos);
        EXPECT_TRUE(contains(call, ", event)"))
            << "a key is sent without its event: " << call;
    }
    EXPECT_GT(calls, 0u);

    const std::string encode = body(src, "QByteArray TerminalWidget::encodeKey(");
    ASSERT_FALSE(encode.empty()) << "encodeKey not found";
    EXPECT_TRUE(contains(encode, "encodeEarlyKey(event)"));
    EXPECT_TRUE(contains(encode, "encodeLegacyKey(event)"));
    const std::string early = body(src, "QByteArray TerminalWidget::encodeEarlyKey(");
    EXPECT_TRUE(contains(early, "bracketedPaste()"));
    EXPECT_TRUE(contains(early, "encodeKittyKey(event)"));
    const std::string legacy = body(src, "QByteArray TerminalWidget::encodeLegacyKey(");
    EXPECT_TRUE(contains(legacy, "applicationCursorKeys()"));
}
