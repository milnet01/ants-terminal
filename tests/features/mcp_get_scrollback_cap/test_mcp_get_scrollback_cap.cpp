// get_scrollback's line cap and truncation marker — see spec.md. ANTS-5219.

#include "remotecontrol.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

#ifndef ANTS_MAINWINDOW_SOURCES
#  error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#  error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {

std::string between(const std::string &s, const std::string &from,
                    const std::string &to) {
    const std::size_t a = s.find(from);
    if (a == std::string::npos) return {};
    const std::size_t b = s.find(to, a + from.size());
    if (b == std::string::npos) return {};
    return s.substr(a, b - a);
}

std::string squash(const std::string &s) {
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out.push_back(c);
    return out;
}

std::size_t count(const std::string &hay, const std::string &needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// The get_scrollback provider, comments stripped: from its registration to the
// next provider's.
std::string provider() {
    const std::string mw = ants_test::stripComments(ants_test::slurpMainWindow());
    return between(mw, "registerToolProvider(\"get_scrollback\"",
                   "registerToolProvider(");
}

}  // namespace

// INV-1
TEST(McpGetScrollbackCap, Inv1RequestIsCapped) {
    const int cap = RemoteControl::kGetTextMaxLines;
    struct Case { int requested, available, fallback, lines, capped; };
    const Case cases[] = {
        {50, 100000, 0, 50, 0},
        {cap + 500, 100000, 0, cap, 500},
        {cap + 500, 200, 0, cap, 0},
        {cap + 500, cap + 100, 0, cap, 100},
        {0, 100000, 0, 0, 0},
        {-3, 100000, 0, 0, 0},
        {0, 100000, 100, 100, 0},
    };
    for (const Case &c : cases) {
        const auto r = RemoteControl::capScrollbackRequest(c.requested, c.available,
                                                           c.fallback);
        EXPECT_EQ(r.lines, c.lines) << "INV-1: lines for requested " << c.requested
                                    << ", available " << c.available;
        EXPECT_EQ(r.linesCapped, c.capped) << "INV-1: linesCapped for requested "
                                           << c.requested << ", available "
                                           << c.available;
    }
}

// INV-2
TEST(McpGetScrollbackCap, Inv2PlainReplyMarksACap) {
    const std::string p = provider();
    ASSERT_FALSE(p.empty()) << "setup: the get_scrollback provider was not found";
    const std::size_t marker = p.find("<capped at");
    ASSERT_NE(marker, std::string::npos)
        << "INV-2: the plain reply has no cap marker";
    const std::size_t guard = p.rfind("linesCapped > 0", marker);
    EXPECT_TRUE(guard != std::string::npos && marker - guard < 200)
        << "INV-2: the cap marker is not guarded by linesCapped > 0";
}

// INV-3
TEST(McpGetScrollbackCap, Inv3EveryReadIsByteTrimmed) {
    const std::string p = squash(provider());
    ASSERT_FALSE(p.empty()) << "setup: the get_scrollback provider was not found";
    const std::size_t reads = count(p, "recentOutput(");
    EXPECT_GT(reads, 0u) << "setup: the provider reads no output";
    EXPECT_EQ(count(p, "trimScrollbackForGetText(t->recentOutput("), reads)
        << "INV-3: some recentOutput result skips trimScrollbackForGetText";
    EXPECT_NE(p.find("RemoteControl::kGetTextDefaultBytesCap"), std::string::npos)
        << "INV-3: the trim does not use kGetTextDefaultBytesCap";
}

// INV-4
TEST(McpGetScrollbackCap, Inv4EnvelopeReportsTheCut) {
    const std::string p = squash(provider());
    ASSERT_FALSE(p.empty()) << "setup: the get_scrollback provider was not found";
    EXPECT_NE(p.find("env[QStringLiteral(\"truncated\")]="), std::string::npos)
        << "INV-4: the envelope never sets truncated";
    EXPECT_NE(p.find("\"lines_dropped\""), std::string::npos)
        << "INV-4: the envelope never sets lines_dropped";
    EXPECT_NE(p.find("\"bytes_dropped\""), std::string::npos)
        << "INV-4: the envelope never sets bytes_dropped";
    EXPECT_EQ(count(p, "<cappedat"), 1u)
        << "INV-4: the cap line appears outside the plain reply";
}

// INV-5
TEST(McpGetScrollbackCap, Inv5GetTextSharesTheLineCap) {
    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        ants_test::slurpRemoteControl(), "QJsonDocument RemoteControl::cmdGetText("));
    ASSERT_FALSE(body.empty()) << "setup: cmdGetText body not found";
    EXPECT_NE(body.find("kGetTextMaxLines"), std::string::npos)
        << "INV-5: cmdGetText does not read kGetTextMaxLines";
    EXPECT_EQ(body.find("10000"), std::string::npos)
        << "INV-5: cmdGetText keeps a literal line cap of its own";
}
