// Feature-conformance source-grep for ANTS-3572 — MCP tokens-saved status-bar
// chip + persisted month / YTD / all-time aggregate. See spec.md and
// docs/specs/ANTS-3572.md. Pure math lives in token_usage_engine; this bundle
// locks the cross-file wiring (signals, single reset path, fold home, verb
// fields) that a pure test can't reach.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH required"
#endif
#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH required"
#endif

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
size_t count(const std::string &hay, const std::string &needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

}  // namespace

// TSC-1 (INV-6) — signals declared; the report-valued emit is in recordDispatch.
TEST(TokensSavedChip, SignalsAndSingleEmitHook) {
    const std::string h   = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_TRUE(has(h, "void tokensSavedUpdated(qint64"))
        << "ClaudeIntegration must declare tokensSavedUpdated(qint64)";
    EXPECT_TRUE(has(h, "void tokenSessionEnding()"))
        << "ClaudeIntegration must declare tokenSessionEnding()";
    // The non-zero (report-valued) emit — distinct from the (0) refresh in
    // endTokenSession — proves the live signal fires from the dispatch hook.
    EXPECT_TRUE(has(cpp,
        "emit tokensSavedUpdated(m_tokenUsage.buildReport(false).totalSaved)"))
        << "recordDispatch must emit the live session total (ANTS-3572 INV-6)";
}

// TSC-2 (INV-2/INV-3) — endTokenSession is the sole reset path (emit-before-
// reset), both reset sites delegate, MainWindow folds + connects (Direct).
TEST(TokensSavedChip, UnifiedResetPathAndFoldWiring) {
    const std::string h   = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw  = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // Exactly one m_tokenUsage.reset() in production code — inside
    // endTokenSession() — so every reset is fold-preceded (INV-2).
    EXPECT_EQ(count(cpp, "m_tokenUsage.reset()"), 1u)
        << "m_tokenUsage.reset() must appear exactly once (in endTokenSession)";
    EXPECT_TRUE(has(cpp, "void ClaudeIntegration::endTokenSession()"))
        << "endTokenSession() must be defined";
    // emit-before-reset ordering within endTokenSession's body.
    const auto ets = cpp.find("void ClaudeIntegration::endTokenSession()");
    ASSERT_NE(ets, std::string::npos);
    const auto emitPos  = cpp.find("emit tokenSessionEnding()", ets);
    const auto resetPos = cpp.find("m_tokenUsage.reset()", ets);
    ASSERT_NE(emitPos,  std::string::npos);
    ASSERT_NE(resetPos, std::string::npos);
    EXPECT_LT(emitPos, resetPos)
        << "tokenSessionEnding must be emitted BEFORE reset (INV-3)";

    // Both production reset sites delegate to endTokenSession.
    EXPECT_TRUE(has(h, "resetTokenUsage() { endTokenSession(); }"))
        << "resetTokenUsage() must delegate to endTokenSession()";
    EXPECT_TRUE(has(cpp, "endTokenSession();"))
        << "the initialize handler must call endTokenSession()";

    // MainWindow owns the fold + connects it (default AutoConnection → Direct;
    // the trailing ')' after the slot means no Qt::QueuedConnection arg).
    EXPECT_TRUE(has(mw, "void MainWindow::foldTokenSavingsIntoConfig()"))
        << "MainWindow must define foldTokenSavingsIntoConfig()";
    EXPECT_TRUE(has(mw, "m_claudeIntegration->endTokenSession()"))
        << "closeEvent must call endTokenSession() at quit";
    EXPECT_TRUE(has(mw, "&ClaudeIntegration::tokenSessionEnding,"))
        << "MainWindow must connect tokenSessionEnding";
    EXPECT_TRUE(has(mw, "&MainWindow::foldTokenSavingsIntoConfig)"))
        << "the connect must target the fold slot with no QueuedConnection arg";
}

// TSC-3 (INV-5/INV-11) — chip visibility gates + placement after the context bar.
TEST(TokensSavedChip, VisibilityGuardsAndPlacement) {
    const std::string cpp =
        ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    EXPECT_TRUE(has(cpp, "claudeTokensSavedChipEnabled()"))
        << "update slot must gate on the enabled flag (INV-11)";
    EXPECT_TRUE(has(cpp, "sessionSaved <= 0"))
        << "update slot must hide when the session total is 0 (INV-5)";
    EXPECT_TRUE(has(cpp, "m_tokensSavedChip->hide()"))
        << "update slot must have a hide() path";
    const auto ctx  = cpp.find("addPermanentWidget(m_contextBar)");
    const auto chip = cpp.find("addPermanentWidget(m_tokensSavedChip)");
    ASSERT_NE(ctx,  std::string::npos);
    ASSERT_NE(chip, std::string::npos);
    EXPECT_LT(ctx, chip)
        << "the chip must be added AFTER the context bar (ANTS-3572 § 2.1)";
}

// TSC-4 (INV-7) — no persistence in the dispatch layer.
TEST(TokensSavedChip, NoPersistenceInDispatchLayer) {
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_FALSE(has(cpp, "setClaudeTokensSaved"))
        << "the dispatch layer must not write the aggregate (INV-7)";
}

// TSC-5 (INV-10) — verb fields live in the response, not the input schema.
TEST(TokensSavedChip, VerbFieldsInResponseNotSchema) {
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_TRUE(has(rc, "env[\"month_saved\"]"));
    EXPECT_TRUE(has(rc, "env[\"ytd_saved\"]"));
    EXPECT_TRUE(has(rc, "env[\"lifetime_saved\"]"));
    EXPECT_TRUE(has(rc, "env[\"monthly\"]"));
    // The response field names must NOT appear in the token_usage inputSchema
    // (they are outputs, not call args) — the schema is built in
    // claudeintegration.cpp and never mentions them.
    EXPECT_FALSE(has(ci, "month_saved"))
        << "aggregate fields must not be declared as input args (INV-10)";
}

// TSC-6 (INV-4 / § 2.4) — month bucket key format.
TEST(TokensSavedChip, MonthKeyFormat) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    EXPECT_TRUE(has(mw, ".toString(\"yyyy-MM\")"))
        << "the fold must derive the bucket key via toString(\"yyyy-MM\")";
}

// TSC-7 (INV-12) — the fold never touches failed-byte waste.
TEST(TokensSavedChip, FoldExcludesFailedBytes) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    EXPECT_FALSE(has(mw, "totalFailedBytes"))
        << "gross-saved only: the fold must not reference totalFailedBytes";
}

// TSC-8 (INV-1) — the summary combines stored + live session.
TEST(TokensSavedChip, SummaryAddsSessionToStored) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    EXPECT_TRUE(has(mw, "claudeTokensSavedLifetime"))
        << "summary must read the stored lifetime";
    EXPECT_TRUE(has(mw, "tokenUsageReport(false).totalSaved"))
        << "summary must add the live session total (INV-1 +session term)";
}
