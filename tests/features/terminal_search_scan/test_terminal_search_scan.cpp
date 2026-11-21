// ANTS-2000 — terminal search results are fixed by the grid, not by how the
// scan reads it. Contract: tests/features/terminal_search_scan/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>

#include <string>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

namespace {

// A widget with a small grid, fed through the real parser, whose search bar is
// driven the way a user drives it.
struct Harness {
    TerminalWidget w;
    VtParser parser{[this](const VtAction &a) { w.grid()->processAction(a); }};
    QLineEdit *input = nullptr;
    QLabel *label = nullptr;
    QPushButton *regexBtn = nullptr;

    Harness() {
        w.grid()->resize(4, 20);
        QWidget *bar = w.findChild<QWidget *>(QStringLiteral("searchBar"));
        if (!bar) return;
        input = bar->findChild<QLineEdit *>();
        for (QLabel *l : bar->findChildren<QLabel *>())
            if (l->text().contains(QLatin1Char('/'))) label = l;
        for (QPushButton *b : bar->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral(".*")) regexBtn = b;
    }
    bool ready() const { return input && label && regexBtn; }
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }

    // Regex mode ON scans synchronously; the label's total is the match count.
    long count(const QString &pattern) {
        input->setText(pattern);
        if (regexBtn->isChecked()) regexBtn->toggle();
        regexBtn->toggle();
        const QString t = label->text();
        const int slash = t.indexOf(QLatin1Char('/'));
        return slash < 0 ? -1 : t.mid(slash + 1).toLong();
    }
};

}  // namespace

TEST(TerminalSearchScan, ResultsMatchTheGrid) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";

    h.feed("NEEDLE one\r\n");
    h.feed("cafe\xCC\x81 bar\r\n");                  // e + U+0301
    h.feed("x\xF0\x9D\x90\x80y\r\n");                // U+1D400 (one cell) then y
    h.feed("\xE4\xB8\x96\xE7\x95\x8C\r\n");          // two wide characters
    for (int i = 0; i < 6; ++i) h.feed("filler line\r\n");
    h.feed("NEEDLE two");
    ASSERT_GT(h.w.grid()->scrollbackSize(), 3) << "corpus did not reach scrollback";
    // A saved line restored narrower than the grid, the way session restore
    // pushes one. resize() pads every history line, so this is the route to
    // cells PAST what a line stores (INV-5).
    {
        TermLine narrow;
        for (char ch : std::string("short")) {
            Cell cell;
            cell.codepoint = static_cast<unsigned char>(ch);
            narrow.cells.push_back(cell);
        }
        h.w.grid()->pushScrollbackLine(std::move(narrow));
    }

    EXPECT_EQ(h.count(QStringLiteral("NEEDLE")), 2) << "INV-1";
    EXPECT_EQ(h.count(QStringLiteral("e\\x{0301}")), 1) << "INV-2";
    EXPECT_EQ(h.count(QStringLiteral("\\x{1D400}y")), 1) << "INV-3";
    EXPECT_EQ(h.count(QStringLiteral("\\x{4E16} \\x{754C}")), 1) << "INV-4";
    EXPECT_EQ(h.count(QStringLiteral("short$")), 1) << "INV-5";
    // An empty match at `o` comes before `one` on the same line.
    EXPECT_EQ(h.count(QStringLiteral("(?=o)|one")), 1) << "INV-6";
    EXPECT_EQ(h.count(QStringLiteral(" $")), 0) << "INV-7: padding matched";
    EXPECT_EQ(h.count(QStringLiteral("short +$")), 0) << "INV-7: padding matched";
    EXPECT_EQ(h.count(QStringLiteral("NEEDLE one$")), 1) << "INV-7: line ends at its last character";
}

// INV-8 (ANTS-6494) — a search keeps at most 100000 matches, the newest ones.
TEST(TerminalSearchScan, MatchListIsCappedKeepingTheNewest) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.w.grid()->resize(4, 200);
    const std::string line = std::string(200, 'a') + "\r\n";
    for (int i = 0; i < 600; ++i) h.feed(line);  // 120000 single-cell matches

    h.input->setText(QStringLiteral("a"));
    if (h.regexBtn->isChecked()) h.regexBtn->toggle();
    h.regexBtn->toggle();
    // Rows 0-2 hold the last three lines; the view centre is row 2, whose
    // first match is the 99801st of the newest 100000.
    EXPECT_EQ(h.label->text().toStdString(), "99801/100000+");
}

// INV-9 (ANTS-5418) — a search longer than one slice yields, then ends where a
// one-slice search does. Each case is 200 lines with a needle on every tenth,
// then the screen's four rows, and is searched whole and in slices.
namespace {
struct SlicedCase {
    const char *name;
    const char *screen;   // the four screen rows
    int sliceLines;
    const char *whole;    // the whole search's counter
};

class SlicedSearch : public ::testing::TestWithParam<SlicedCase> {};
}  // namespace

TEST_P(SlicedSearch, YieldsAndMatchesAWholeOne) {
    const SlicedCase &c = GetParam();
    auto fill = [&c](Harness &x) {
        for (int i = 0; i < 200; ++i) x.feed(i % 10 == 0 ? "NEEDLE\r\n" : "filler\r\n");
        x.feed(c.screen);
    };
    Harness one;
    ASSERT_TRUE(one.ready()) << "search bar widgets not found";
    fill(one);
    ASSERT_GT(one.w.grid()->scrollbackSize(), 150);
    one.count(QStringLiteral("NEEDLE"));
    const std::string whole = one.label->text().toStdString();
    const int wholeOffset = one.w.scrollOffset();
    ASSERT_EQ(whole, c.whole);

    Harness h;
    fill(h);
    h.w.setSearchSliceCellsForTest(c.sliceLines * 20);
    h.count(QStringLiteral("NEEDLE"));
    EXPECT_TRUE(h.label->text().endsWith(QChar(0x2026)))
        << "INV-9: the search did not yield; label " << h.label->text().toStdString();
    for (int i = 0; i < 10000 && h.label->text().endsWith(QChar(0x2026)); ++i)
        QCoreApplication::processEvents();
    EXPECT_EQ(h.label->text().toStdString(), whole) << "INV-9: the sliced result differs";
    EXPECT_EQ(h.w.scrollOffset(), wholeOffset) << "INV-9: the view landed elsewhere";
}

// oldestOnly: no needle at or below the view's centre (row 2), so the current
// match is the oldest, which only the last slice finds. centreRow: needles on
// rows 2 and 3; the first one-line slice finds row 3's, but row 2's is current.
INSTANTIATE_TEST_SUITE_P(
    TerminalSearchScan, SlicedSearch,
    ::testing::Values(SlicedCase{"oldestOnly", "tail\r\ntail\r\ntail\r\ntail", 5, "1/20"},
                      SlicedCase{"centreRow", "tail\r\ntail\r\nNEEDLE c\r\nNEEDLE d", 1,
                                 "21/22"}),
    [](const ::testing::TestParamInfo<SlicedCase> &info) { return info.param.name; });
