// ANTS-4534 — a per-tab scrollback memory cap. Contract: spec.md beside this
// file.

#include "terminalgrid.h"
#include "vtparser.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <string>

namespace {

constexpr int kRows = 4;
constexpr int kCols = 200;
constexpr qint64 kMiB = 1024 * 1024;

struct Term {
    TerminalGrid grid{kRows, kCols};
    VtParser parser{[this](const VtAction &a) { grid.processAction(a); }};
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }
    // `n` full-width lines, each starting with its number so the oldest kept
    // line can be read back.
    void fullLines(int from, int n) {
        for (int i = from; i < from + n; ++i) {
            std::string line = std::to_string(i) + ":";
            line.resize(kCols, 'x');
            feed(line + "\r\n");
        }
    }
};

qint64 storedBytes(const TerminalGrid &g) {
    qint64 total = 0;
    for (int i = 0; i < g.scrollbackSize(); ++i)
        total += static_cast<qint64>(g.scrollbackLine(i).size() * sizeof(Cell));
    return total;
}

int lineNumber(const TerminalGrid &g, int index) {
    std::string s;
    for (const Cell &c : g.scrollbackLine(index)) {
        if (c.codepoint == ':') break;
        s += char(c.codepoint);
    }
    return std::stoi(s);
}

}  // namespace

// INV-1
TEST(ScrollbackMemoryCap, Inv1CapHoldsAndDropsOldest) {
    Term t;
    t.grid.setMaxScrollbackMib(16);
    const qint64 perLine = kCols * static_cast<qint64>(sizeof(Cell));
    const int fit = static_cast<int>(16 * kMiB / perLine);
    t.fullLines(0, fit + 500);
    EXPECT_LE(t.grid.scrollbackBytes(), 16 * kMiB);
    EXPECT_LT(t.grid.scrollbackSize(), t.grid.maxScrollback())
        << "setup: the line limit must not be what evicts";
    EXPECT_GT(lineNumber(t.grid, 0), 0) << "the oldest lines are the ones dropped";
    EXPECT_EQ(lineNumber(t.grid, t.grid.scrollbackSize() - 1),
              fit + 500 - kRows) << "the newest pushed line is kept";
}

// INV-2
TEST(ScrollbackMemoryCap, Inv2LoweringTrimsAtOnce) {
    Term t;
    t.grid.setMaxScrollbackMib(64);
    t.fullLines(0, static_cast<int>(32 * kMiB / (kCols * sizeof(Cell))));
    ASSERT_GT(t.grid.scrollbackBytes(), 16 * kMiB) << "setup: fill past the new cap";
    t.grid.setMaxScrollbackMib(16);
    EXPECT_LE(t.grid.scrollbackBytes(), 16 * kMiB);
}

// INV-3
TEST(ScrollbackMemoryCap, Inv3CountMatchesStorage) {
    Term t;
    t.fullLines(0, 50);
    t.feed("short\r\n\r\n");
    EXPECT_EQ(t.grid.scrollbackBytes(), storedBytes(t.grid)) << "after pushes";
    t.grid.resize(kRows, 80);
    EXPECT_EQ(t.grid.scrollbackBytes(), storedBytes(t.grid)) << "after a narrowing resize";
    t.grid.resize(kRows, kCols);
    EXPECT_EQ(t.grid.scrollbackBytes(), storedBytes(t.grid)) << "after a widening resize";
    t.feed("\x1b[3J");
    EXPECT_EQ(t.grid.scrollbackSize(), 0);
    EXPECT_EQ(t.grid.scrollbackBytes(), 0) << "after ED 3";
    t.fullLines(0, 10);
    t.feed("\x1b" "c");
    EXPECT_EQ(t.grid.scrollbackBytes(), storedBytes(t.grid)) << "after a reset";
}

// INV-4
TEST(ScrollbackMemoryCap, Inv4ResetKeepsTheCap) {
    Term t;
    t.grid.setMaxScrollbackMib(100);
    t.feed("\x1b" "c");
    EXPECT_EQ(t.grid.maxScrollbackMib(), 100);
}

// INV-5
TEST(ScrollbackMemoryCap, Inv5RangeAndDefault) {
    TerminalGrid g(kRows, kCols);
    EXPECT_EQ(g.maxScrollbackMib(), 256);
    g.setMaxScrollbackMib(1);
    EXPECT_EQ(g.maxScrollbackMib(), 16);
    g.setMaxScrollbackMib(1'000'000);
    EXPECT_EQ(g.maxScrollbackMib(), 65536);

    const std::string cfg = ants_test::slurpFile(ANTS_SRC_DIR "/config.cpp");
    EXPECT_NE(cfg.find("m_data.value(\"scrollback_max_mib\").toInt(256)"), std::string::npos);
}

// INV-6
TEST(ScrollbackMemoryCap, Inv6WiredThroughConfigAndSettings) {
    const std::string tabs = ants_test::slurpFile(ANTS_SRC_DIR "/mainwindow_tabs.cpp");
    EXPECT_NE(tabs.find("terminal->setMaxScrollbackMib(m_config.scrollbackMaxMib());"),
              std::string::npos);
    const std::string dlg = ants_test::slurpFile(ANTS_SRC_DIR "/settingsdialog.cpp");
    EXPECT_NE(dlg.find("m_scrollbackMaxMib->setValue(m_config->scrollbackMaxMib());"),
              std::string::npos);
    EXPECT_NE(dlg.find("m_config->setScrollbackMaxMib(m_scrollbackMaxMib->value());"),
              std::string::npos);
}
