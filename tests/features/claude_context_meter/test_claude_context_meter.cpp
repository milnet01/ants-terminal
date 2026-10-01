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
    EXPECT_EQ(snap.contextTokens, 456789);
}

// INV-2
TEST(ClaudeContextMeter, Inv2PercentAgainstWindow) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ClaudeIntegration ci;
    EXPECT_EQ(ci.contextWindowTokens(), 1'000'000);
    QSignalSpy spy(&ci, &ClaudeIntegration::contextUpdated);
    ci.parseTranscriptForState(writeTranscript(tmp));
    EXPECT_EQ(ci.contextTokens(), 456789);
    EXPECT_EQ(ci.contextPercent(), 45);   // 456789 * 100 / 1,000,000
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
