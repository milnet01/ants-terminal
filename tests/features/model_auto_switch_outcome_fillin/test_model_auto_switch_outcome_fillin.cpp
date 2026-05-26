// ANTS-1735 §2.5 outcome fill-in tick wiring guard. The pure
// `ModelSwitchLedger::computeOutcome` helper is exercised by
// tests/features/model_switch_ledger/; this test source-greps the
// controller method and its mainwindow timer wire-site.

#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string between(const std::string &src, const std::string &begin,
                    const std::string &end) {
    const auto b = src.find(begin);
    if (b == std::string::npos) return {};
    const auto e = src.find(end, b + begin.size());
    if (e == std::string::npos) return {};
    return src.substr(b, e - b);
}

}  // namespace

// fillPendingLedgerOutcomes exists and is throttled via m_lastPendingFillMs +
// kPendingFillIntervalMs (the §2.5 cadence — 30 s).
TEST(ModelAutoSwitchOutcomeFillin, ThrottleBail) {
    const std::string src = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    const std::string body = between(
        src, "void ClaudeStatusBarController::fillPendingLedgerOutcomes()",
        "} // anonymous"  // sentinel — match end of file or any sentinel block
    );
    // If the sentinel didn't match, the method body still appears in src;
    // search the whole file as a fallback.
    const std::string scope = body.empty() ? src : body;
    EXPECT_NE(scope.find("m_lastPendingFillMs"), std::string::npos)
        << "fill-in must throttle via m_lastPendingFillMs";
    EXPECT_NE(scope.find("kPendingFillIntervalMs"), std::string::npos)
        << "fill-in throttle interval must reference kPendingFillIntervalMs";
}

// The pure helper is the entry point — no inline reimplementation.
TEST(ModelAutoSwitchOutcomeFillin, CallsComputeOutcome) {
    const std::string src = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_NE(src.find("ModelSwitchLedger::computeOutcome("), std::string::npos)
        << "fill-in must delegate to ModelSwitchLedger::computeOutcome";
    EXPECT_NE(src.find("ModelSwitchLedger::writeRecords("), std::string::npos)
        << "fill-in must persist via writeRecords";
}

// Read-only when nothing changed — must check `fill.changed` before writing.
TEST(ModelAutoSwitchOutcomeFillin, WriteOnlyWhenChanged) {
    const std::string src = slurp(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    // The body should reference `anyChanged` or `fill.changed` and gate
    // writeRecords on it — no unconditional writes per tick.
    // Method is the last definition in the file; slice from the function
    // header to EOF and assert against that scope.
    const std::string marker =
        "void ClaudeStatusBarController::fillPendingLedgerOutcomes()";
    const auto bodyStart = src.find(marker);
    ASSERT_NE(bodyStart, std::string::npos) << "method definition not found";
    const std::string body = src.substr(bodyStart);
    EXPECT_TRUE(body.find("anyChanged") != std::string::npos ||
                body.find(".changed")    != std::string::npos)
        << "writeRecords must be gated on a changed flag — no unconditional "
           "per-tick writes";
    const auto writePos = body.find("ModelSwitchLedger::writeRecords(");
    ASSERT_NE(writePos, std::string::npos);
    const std::string preWrite = body.substr(0, writePos);
    EXPECT_TRUE(preWrite.find("if (anyChanged") != std::string::npos ||
                preWrite.find("if(anyChanged")  != std::string::npos)
        << "writeRecords must sit inside an `if (anyChanged)` guard";
}

// MainWindow wires the fill-in onto the 2 s status timer next to
// refreshAutoModelSwitch (same connect block).
TEST(ModelAutoSwitchOutcomeFillin, WiredOnStatusTimer) {
    const std::string src = slurp(SRC_MAINWINDOW_CPP_PATH);
    const auto wirePos = src.find("fillPendingLedgerOutcomes");
    ASSERT_NE(wirePos, std::string::npos)
        << "fillPendingLedgerOutcomes must be referenced in MainWindow";
    const size_t start = wirePos > 600 ? wirePos - 600 : 0;
    const std::string window = src.substr(start, wirePos - start);
    EXPECT_NE(window.find("m_statusTimer"), std::string::npos)
        << "fill-in connect must reference m_statusTimer (the 2 s tick)";
}
