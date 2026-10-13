// Claude context meter — contract: spec.md here.

#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include "claudeintegration.h"
#include "claudestatuswidgets.h"
#include "config.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <string>

#ifndef SRC_CLAUDESTATUSWIDGETS_CPP_PATH
#  error "SRC_CLAUDESTATUSWIDGETS_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDEINTEGRATION_CPP_PATH
#  error "SRC_CLAUDEINTEGRATION_CPP_PATH compile definition required"
#endif

namespace {

// The usage figures of a real Opus session (2026-10-01), where input_tokens
// alone is 2.
QString writeTranscript(const QTemporaryDir &tmp) {
    QString path = tmp.path() + "/ctx.jsonl";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(R"({"type":"assistant","message":{"stop_reason":"end_turn","usage":)"
            R"({"input_tokens":2,"cache_creation_input_tokens":153,)"
            R"("cache_read_input_tokens":456634,"output_tokens":338},)"
            R"("content":[{"type":"text","text":"x"}]}})");
    f.close();
    return path;
}

}  // namespace

// INV-1
TEST(ClaudeContextMeter, Inv1TokensSumAllThreeFields) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto snap = ClaudeIntegration::parseTranscriptTail(writeTranscript(tmp), false);
    EXPECT_EQ(snap.contextTokens, 457127);   // + output_tokens 338
}

// INV-7 (ANTS-5631) — a compact boundary newer than every usage event is
// the reading: its compactMetadata.postTokens. The record shapes are a
// real auto-compact's (2026-10-13), trimmed to the fields read here.
TEST(ClaudeContextMeter, Inv7CompactBoundaryIsTheNewestReading) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + "/compact.jsonl";
    const QByteArray before =
        R"({"type":"assistant","message":{"stop_reason":"end_turn","usage":)"
        R"({"input_tokens":2,"cache_creation_input_tokens":1000,)"
        R"("cache_read_input_tokens":300511,"output_tokens":40}}})" "\n"
        R"({"type":"system","subtype":"compact_boundary","compactMetadata":)"
        R"({"trigger":"auto","preTokens":301513,"postTokens":24450}})" "\n"
        R"({"type":"user","isCompactSummary":true,"message":{"role":"user",)"
        R"("content":"summary"}})" "\n";
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(before);
    }
    EXPECT_EQ(ClaudeIntegration::parseTranscriptTail(path, false).contextTokens,
              24450)
        << "the pre-compact figure survived the compact";

    // The first reply after the compact is newer again, so it wins.
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::Append));
        f.write(R"({"type":"assistant","message":{"stop_reason":"end_turn",)"
                R"("usage":{"input_tokens":2,"cache_creation_input_tokens":55096,)"
                R"("cache_read_input_tokens":32986,"output_tokens":315}}})" "\n");
    }
    EXPECT_EQ(ClaudeIntegration::parseTranscriptTail(path, false).contextTokens,
              88399);   // + output_tokens 315
}

// INV-9 (ANTS-5682) — records after the newest reply are estimated at four
// bytes a token, from their text alone, until the next reply replaces them.
TEST(ClaudeContextMeter, Inv9EstimatesRecordsAfterTheLastReply) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = writeTranscript(tmp);   // 457127 with output
    const QByteArray output(40000, 'a');
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::Append));
        // A tool result as Claude Code writes it: the output in the message
        // and again in toolUseResult, which is not sent to the model.
        f.write("\n" R"({"type":"user","message":{"role":"user","content":[)"
                R"({"type":"tool_result","tool_use_id":"t1","content":")"
                + output + R"("}]},"toolUseResult":{"stdout":")" + output
                + R"("}})" "\n");
    }
    auto snap = ClaudeIntegration::parseTranscriptTail(path, false);
    EXPECT_EQ(snap.contextTokens, 457127 + 10000)
        << "the tool output is counted once, at bytes / 4";
    EXPECT_TRUE(snap.contextEstimated);

    // The next reply is the real figure again.
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::Append));
        f.write(R"({"type":"assistant","message":{"stop_reason":"end_turn",)"
                R"("usage":{"input_tokens":2,"cache_creation_input_tokens":12000,)"
                R"("cache_read_input_tokens":456634,"output_tokens":100}}})" "\n");
    }
    snap = ClaudeIntegration::parseTranscriptTail(path, false);
    EXPECT_EQ(snap.contextTokens, 468736);
    EXPECT_FALSE(snap.contextEstimated);
}

// INV-8 (ANTS-5683) — clearing the reading zeroes it and tells the meter.
TEST(ClaudeContextMeter, Inv8ClearContextReadingHidesTheMeter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ClaudeIntegration ci;
    ci.parseTranscriptForState(writeTranscript(tmp));
    ASSERT_EQ(ci.contextTokens(), 457127);
    QSignalSpy spy(&ci, &ClaudeIntegration::contextUpdated);
    ci.clearContextReading();
    EXPECT_EQ(ci.contextTokens(), 0);
    EXPECT_EQ(ci.contextPercent(), 0);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toInt(), 0);
    // Nothing to clear: no signal, so a poll with no Claude stays quiet.
    ci.clearContextReading();
    EXPECT_EQ(spy.count(), 0);
}

// INV-8 — pollClaudeProcess clears it when Claude exits and when another
// Claude process replaces it. Source scrape: the poll walks /proc for a real
// child named claude, which a unit test cannot stage.
TEST(ClaudeContextMeter, Inv8PollClearsOnExitAndOnNewProcess) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDEINTEGRATION_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t at = src.find("void ClaudeIntegration::pollClaudeProcess() {");
    ASSERT_NE(at, std::string::npos);
    const std::string body = src.substr(at, src.find("\n}\n", at) - at);

    const size_t gone = body.find("if (!found) {");
    ASSERT_NE(gone, std::string::npos);
    const std::string goneBlock =
        body.substr(gone, body.find("return;", gone) - gone);
    EXPECT_NE(goneBlock.find("clearContextReading();"), std::string::npos)
        << "Claude exited, but the meter keeps its reading";

    const size_t changed = body.find("if (pidChanged) {");
    ASSERT_NE(changed, std::string::npos);
    const std::string changedBlock =
        body.substr(changed, body.find("\n    }\n", changed) - changed);
    EXPECT_NE(changedBlock.find("clearContextReading();"), std::string::npos)
        << "a new Claude process inherits the old session's reading";
}

// INV-2
TEST(ClaudeContextMeter, Inv2PercentAgainstWindow) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ClaudeIntegration ci;
    EXPECT_EQ(ci.contextWindowTokens(), 1'000'000);
    QSignalSpy spy(&ci, &ClaudeIntegration::contextUpdated);
    ci.parseTranscriptForState(writeTranscript(tmp));
    EXPECT_EQ(ci.contextTokens(), 457127);
    EXPECT_EQ(ci.contextPercent(), 45);   // 457127 * 100 / 1,000,000
    ASSERT_GE(spy.count(), 1);

    spy.clear();
    ci.setContextWindowTokens(500'000);
    EXPECT_EQ(ci.contextPercent(), 91);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toInt(), 91);

    ci.setContextWindowTokens(200'000);
    EXPECT_EQ(ci.contextPercent(), 100);  // capped
}

// INV-3
TEST(ClaudeContextMeter, Inv3ConfigDefaultAndClamp) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants_test::XdgGuard g;
    g.setTestMode(false);   // test mode would ignore XDG_CONFIG_HOME
    g.setEnv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
    {
        Config cfg;
        EXPECT_EQ(cfg.claudeContextWindowTokens(), 1'000'000);
        cfg.setClaudeContextWindowTokens(250'000);
    }
    {
        Config cfg;
        EXPECT_EQ(cfg.claudeContextWindowTokens(), 250'000);
        cfg.setClaudeContextWindowTokens(5);
        EXPECT_EQ(cfg.claudeContextWindowTokens(), Config::kMinContextWindowTokens);
        cfg.setClaudeContextWindowTokens(2'000'000'000);
        EXPECT_EQ(cfg.claudeContextWindowTokens(), Config::kMaxContextWindowTokens);
    }
}

// INV-4
TEST(ClaudeContextMeter, Inv4TooltipNamesTheNumbers) {
    const QString low = contextMeterTooltip(456789, 1'000'000, 45);
    EXPECT_TRUE(low.contains(QStringLiteral("456,789"))) << low.toStdString();
    EXPECT_TRUE(low.contains(QStringLiteral("1,000,000"))) << low.toStdString();
    EXPECT_TRUE(low.contains(QStringLiteral("45%"))) << low.toStdString();
    EXPECT_FALSE(low.contains(QStringLiteral("/compact"))) << low.toStdString();
    const QString high = contextMeterTooltip(850000, 1'000'000, 85);
    EXPECT_TRUE(high.contains(QStringLiteral("/compact"))) << high.toStdString();
    // ANTS-5682 — an estimate says so; an exact figure does not.
    EXPECT_FALSE(low.contains(QStringLiteral("Estimated"))) << low.toStdString();
    const QString est = contextMeterTooltip(456789, 1'000'000, 45, true);
    EXPECT_TRUE(est.contains(QStringLiteral("Estimated"))) << est.toStdString();
}

// INV-5 — the handler keys visibility on the token count.
TEST(ClaudeContextMeter, Inv5ShownOnTokensNotPercent) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t at = src.find("&ClaudeIntegration::contextUpdated");
    ASSERT_NE(at, std::string::npos);
    const std::string handler = src.substr(at, src.find("});", at) - at);
    EXPECT_NE(handler.find("contextTokens() <= 0"), std::string::npos)
        << "the meter must hide on no tokens, not on a 0% reading";
    EXPECT_EQ(handler.find("percent <= 0"), std::string::npos)
        << "a small session rounds to 0% and must still show";
}

// INV-5 — apply() hides the meter on Display::Hidden, so its visible path
// must re-show it. Otherwise the meter stays gone after a brief state blip
// until the next contextUpdated, which needs a new assistant reply
// (ANTS-5630).
TEST(ClaudeContextMeter, Inv5ShownAgainAfterHidden) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t at = src.find("void ClaudeStatusBarController::apply() {");
    ASSERT_NE(at, std::string::npos);
    const std::string body = src.substr(at, src.find("\n}\n", at) - at);
    const size_t hidden = body.find("if (d == claudestate::Display::Hidden)");
    ASSERT_NE(hidden, std::string::npos);
    const size_t visible = body.find("return;", hidden);
    ASSERT_NE(visible, std::string::npos);
    const std::string rest = body.substr(visible);
    EXPECT_NE(rest.find("contextTokens() > 0"), std::string::npos)
        << "the visible path must check for a known token count";
    EXPECT_NE(rest.find("m_contextBar->show()"), std::string::npos)
        << "the visible path must re-show the meter it hid";
}

// INV-6 — size, weight and outline.
TEST(ClaudeContextMeter, Inv6ReadableMeter) {
    const std::string src = ants_test::slurpFile(SRC_CLAUDESTATUSWIDGETS_CPP_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("class ContextMeter"), std::string::npos);
    EXPECT_NE(src.find("QStringLiteral(\"Context %1%\")"), std::string::npos);
    EXPECT_NE(src.find("setBold(true)"), std::string::npos);
    EXPECT_NE(src.find("setPixelSize(13)"), std::string::npos);
    EXPECT_NE(src.find("strokePath"), std::string::npos)
        << "the text needs a dark outline drawn under it";
    EXPECT_NE(src.find("setFixedSize(160, 22)"), std::string::npos);
}
