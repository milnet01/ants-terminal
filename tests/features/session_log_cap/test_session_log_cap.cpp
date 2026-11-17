// Feature-conformance test for ANTS-6493 — each tab's session log stops at
// 1 GB, says so, and keeps what it saved.
// Contract: tests/features/session_log_cap/spec.md

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "terminalwidget.h"

#include <string>

namespace {

std::string body(const char *signature) {
    static const std::string src =
        ants_test::stripComments(ants_test::slurpFile(SRC_TERMINALWIDGET_PATH));
    return ants_test::slurpFunctionBody(src, signature);
}

}  // namespace

// INV-1
TEST(SessionLogCap, RoomIsWhatFitsUnderTheCap) {
    EXPECT_EQ(TerminalWidget::sessionLogRoom(0, 10, 100), 10);
    EXPECT_EQ(TerminalWidget::sessionLogRoom(90, 10, 100), 10) << "exactly fills it";
    EXPECT_EQ(TerminalWidget::sessionLogRoom(95, 10, 100), 5) << "the remainder";
    EXPECT_EQ(TerminalWidget::sessionLogRoom(100, 10, 100), 0) << "full";
    EXPECT_EQ(TerminalWidget::sessionLogRoom(120, 10, 100), 0) << "never negative";
    EXPECT_EQ(TerminalWidget::sessionLogRoom(50, 0, 100), 0);
}

// INV-2
TEST(SessionLogCap, CapIsOneGigabyte) {
    EXPECT_EQ(TerminalWidget::kSessionLogCapBytes, 1'000'000'000);
}

// INV-3
TEST(SessionLogCap, BatchWriteStopsAtTheCapAndSaysSo) {
    const std::string vt = body("void TerminalWidget::onVtBatch(");
    ASSERT_FALSE(vt.empty());
    EXPECT_NE(vt.find("sessionLogRoom("), std::string::npos);
    EXPECT_EQ(vt.find("m_logFile->write(batch->rawBytes)"), std::string::npos)
        << "an unbounded write of the whole batch is the defect";
    const std::string full = ants_test::regionBetween(vt, "sessionLogRoom(", "m_recording");
    EXPECT_NE(full.find("m_logFile->close()"), std::string::npos);
    EXPECT_NE(full.find("m_loggingEnabled = false"), std::string::npos);
    EXPECT_NE(full.find("emit captureFailed("), std::string::npos);
    EXPECT_NE(full.find("full"), std::string::npos) << "the message says the log is full";
}

// INV-4
TEST(SessionLogCap, NewLogStartsAtZero) {
    const std::string set = body("void TerminalWidget::setSessionLogging(");
    ASSERT_FALSE(set.empty());
    EXPECT_NE(set.find("m_logBytes = 0"), std::string::npos);
}
