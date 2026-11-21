// Search matches follow their lines when scrollback drops its oldest.
// Contract: tests/features/search_matches_follow_scrollback/spec.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"
#include "vtstream.h"

namespace {

constexpr int kCap = 1000;   // the smallest scrollback cap the grid accepts
constexpr int kRows = 4;

struct Harness {
    TerminalWidget w;
    QLineEdit *input = nullptr;
    QPushButton *regexBtn = nullptr;
    QWidget *bar = nullptr;

    Harness() {
        w.grid()->resize(kRows, 40);
        w.setMaxScrollback(kCap);
        bar = w.findChild<QWidget *>(QStringLiteral("searchBar"));
        if (!bar) return;
        input = bar->findChild<QLineEdit *>();
        for (QPushButton *b : bar->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral(".*")) regexBtn = b;
    }
    bool ready() const { return input && regexBtn; }

    // The "current/total" label: the bar's one label with a slash in it.
    QString counterText() const {
        for (const QLabel *l : bar->findChildren<QLabel *>())
            if (l->text().contains(QLatin1Char('/'))) return l->text();
        return {};
    }

    // Output as the widget receives it: one parsed batch through onVtBatch.
    // `s` outlives the call, because a print run points into it.
    void feed(const std::string &s) {
        auto batch = std::make_shared<VtBatch>();
        VtParser parser([&batch](const VtAction &a) { batch->actions.push_back(a); });
        parser.feed(s.data(), static_cast<int>(s.size()));
        batch->rawBytes = QByteArray(s.data(), static_cast<qsizetype>(s.size()));
        VtBatchPtr shipped = batch;
        ASSERT_TRUE(QMetaObject::invokeMethod(&w, "onVtBatch", Qt::DirectConnection,
                                              Q_ARG(VtBatchPtr, shipped)));
    }

    // Regex mode ON scans synchronously.
    void search(const QString &pattern) {
        input->setText(pattern);
        if (regexBtn->isChecked()) regexBtn->toggle();
        regexBtn->toggle();
    }

    int totalLines() { return w.grid()->scrollbackSize() + kRows; }

    // Global lines that carry a stored match, the dropped range included.
    std::vector<int> matchedLines() {
        std::vector<int> out;
        for (int gl = -kCap; gl < totalLines(); ++gl)
            if (!w.searchMatchColumnsForTest(gl).empty()) out.push_back(gl);
        return out;
    }

    // Scrollback lines whose cells start with the ASCII `text`, from the grid.
    std::vector<int> scrollbackLinesStarting(const std::string &text) {
        std::vector<int> out;
        const TerminalGrid *g = w.grid();
        for (int i = 0; i < g->scrollbackSize(); ++i) {
            const auto &cells = g->scrollbackLine(i);
            bool hit = cells.size() >= text.size();
            for (std::size_t k = 0; k < text.size() && hit; ++k)
                hit = cells[k].codepoint == static_cast<unsigned char>(text[k]);
            if (hit) out.push_back(i);
        }
        return out;
    }
};

std::string lines(int from, int count) {
    std::string s;
    for (int i = from; i < from + count; ++i) s += "row" + std::to_string(i) + "\r\n";
    return s;
}

}  // namespace

TEST(SearchMatchesFollowScrollback, MatchMovesWithItsLine) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    const std::string before = lines(0, 600) + "NEEDLE here\r\n" + lines(600, 600);
    h.feed(before);
    ASSERT_EQ(h.w.grid()->scrollbackSize(), kCap) << "scrollback is not at its cap";

    h.search(QStringLiteral("NEEDLE"));
    const std::vector<int> at = h.scrollbackLinesStarting("NEEDLE");
    ASSERT_EQ(at.size(), 1u);
    ASSERT_EQ(h.matchedLines(), at) << "the search did not land on the needle's line";

    const std::string more = lines(2000, 10);
    h.feed(more);
    const std::vector<int> now = h.scrollbackLinesStarting("NEEDLE");
    ASSERT_EQ(now.size(), 1u);
    ASSERT_EQ(now.front(), at.front() - 10) << "the fixture did not drop ten lines";
    EXPECT_EQ(h.matchedLines(), now)
        << "INV-1: the match is not on the line that holds the needle";
}

TEST(SearchMatchesFollowScrollback, MatchOnADroppedLineIsRemoved) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    // After this feed the first needle is among the oldest kept lines.
    const std::string before =
        lines(0, 203) + "NEEDLE old\r\n" + lines(203, 500) + "NEEDLE kept\r\n" + lines(703, 500);
    h.feed(before);
    ASSERT_EQ(h.w.grid()->scrollbackSize(), kCap);
    h.search(QStringLiteral("NEEDLE"));
    const std::vector<int> at = h.scrollbackLinesStarting("NEEDLE");
    ASSERT_EQ(at.size(), 2u);
    ASSERT_LT(at.front(), 10) << "the old needle is not within ten lines of the front";
    ASSERT_EQ(h.matchedLines(), at);

    const std::string more = lines(2000, 10);
    h.feed(more);
    const std::vector<int> now = h.scrollbackLinesStarting("NEEDLE");
    ASSERT_EQ(now.size(), 1u) << "the fixture did not drop the old needle";
    EXPECT_EQ(h.matchedLines(), now) << "INV-2: a match outlived its line";
    EXPECT_EQ(h.counterText().toStdString(), "1/1") << "INV-2: the counter";
}

// Every global line, scrollback and screen, whose cells start with `text`.
static std::vector<int> linesStarting(TerminalWidget &w, const std::string &text) {
    std::vector<int> out;
    const TerminalGrid *g = w.grid();
    const int sb = g->scrollbackSize();
    for (int gl = 0; gl < sb + kRows; ++gl) {
        bool hit = true;
        for (std::size_t k = 0; k < text.size() && hit; ++k) {
            const auto cp = static_cast<unsigned char>(text[k]);
            const int col = static_cast<int>(k);
            if (gl < sb) {
                const auto &cells = g->scrollbackLine(gl);
                hit = col < static_cast<int>(cells.size()) && cells[k].codepoint == cp;
            } else {
                hit = g->cellAt(gl - sb, col).codepoint == cp;
            }
        }
        if (hit) out.push_back(gl);
    }
    return out;
}

TEST(SearchMatchesFollowScrollback, NewOutputIsMatched) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.feed(lines(0, 20) + "NEEDLE one\r\n");
    h.search(QStringLiteral("NEEDLE"));
    ASSERT_EQ(h.counterText().toStdString(), "1/1");

    h.feed("NEEDLE two\r\n" + lines(100, 10));
    const std::vector<int> at = linesStarting(h.w, "NEEDLE");
    ASSERT_EQ(at.size(), 2u);
    EXPECT_EQ(h.matchedLines(), at) << "INV-3: new output was not matched";
    EXPECT_EQ(h.counterText().toStdString(), "1/2") << "INV-3: the counter";
}

TEST(SearchMatchesFollowScrollback, RewrittenLineLosesItsMatch) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.feed(lines(0, 20) + "NEEDLE");
    h.search(QStringLiteral("NEEDLE"));
    ASSERT_EQ(h.matchedLines().size(), 1u);

    h.feed("\r\x1b[2Kplain");
    EXPECT_TRUE(h.matchedLines().empty()) << "INV-4: a rewritten line kept its match";
    EXPECT_EQ(h.counterText().toStdString(), "0/0") << "INV-4: the counter";
}

TEST(SearchMatchesFollowScrollback, CurrentMatchAndViewStay) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.feed(lines(0, 50) + "NEEDLE a\r\n" + lines(50, 50));
    h.search(QStringLiteral("NEEDLE"));
    ASSERT_EQ(h.counterText().toStdString(), "1/1");
    ASSERT_GT(h.w.scrollOffset(), 0) << "the search did not scroll back to its match";
    const int topBefore = h.w.grid()->scrollbackSize() - h.w.scrollOffset();

    h.feed("NEEDLE b\r\n" + lines(200, 5));
    EXPECT_EQ(h.counterText().toStdString(), "1/2")
        << "INV-5: the current match moved, or the new one was not counted";
    EXPECT_EQ(h.w.grid()->scrollbackSize() - h.w.scrollOffset(), topBefore)
        << "INV-5: the view moved";
}

// INV-6 (ANTS-6494) — new output past the cap drops the oldest matches.
TEST(SearchMatchesFollowScrollback, NewOutputPastTheCapDropsTheOldest) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.w.grid()->resize(kRows, 200);
    const std::string aLine = std::string(200, 'a') + "\r\n";
    std::string block;
    for (int i = 0; i < 600; ++i) block += aLine;  // 120000 single-cell matches
    h.feed(block);
    h.search(QStringLiteral("a"));
    ASSERT_EQ(h.counterText().toStdString(), "99801/100000+");

    std::string more;
    for (int i = 0; i < 100; ++i) more += aLine;
    h.feed(more);
    const QString counter = h.counterText();
    EXPECT_TRUE(counter.endsWith(QStringLiteral("/100000+"))) << counter.toStdString();
    // The cursor sits on an empty last row; the line above it is the newest.
    EXPECT_FALSE(h.w.searchMatchColumnsForTest(h.totalLines() - 2).empty())
        << "INV-6: the newest line lost its matches";
}

// INV-7 (ANTS-5418) — output while a sliced search runs leaves it matching
// every line that holds the pattern, and nothing else.
// One line per slice: the output lands on lines the scan has not reached.
// Fifty: it lands below the scan, which then meets the shifted lines.
namespace {
class OutputDuringASlicedSearch : public ::testing::TestWithParam<int> {};
}  // namespace

TEST_P(OutputDuringASlicedSearch, MatchesEndEqualToTheGrid) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    std::string before;
    // 1202 lines leave needle 1200 on screen, where the output lands.
    for (int i = 0; i < 1202; ++i)
        before += (i % 10 == 0 ? "NEEDLE " : "row") + std::to_string(i) + "\r\n";
    h.feed(before);
    ASSERT_EQ(h.w.grid()->scrollbackSize(), kCap);

    h.w.setSearchSliceCellsForTest(40 * GetParam());
    h.search(QStringLiteral("NEEDLE"));
    const auto running = [&h] { return h.counterText().endsWith(QChar(0x2026)); };
    ASSERT_TRUE(running()) << "the search finished in one slice";
    if (GetParam() > 1) QCoreApplication::processEvents();
    // Drops 30 lines off the front, three of them needles, and prints two
    // more. Any 30 lines hold three needles, so a scan that lost its place
    // by the drop would count some twice.
    h.feed(lines(5000, 10) + "NEEDLE new1\r\n" + lines(5010, 8) + "NEEDLE new2\r\n" +
           lines(5020, 10));
    for (int i = 0; i < 10000 && running(); ++i) QCoreApplication::processEvents();
    ASSERT_FALSE(running()) << "the search never finished";

    const std::vector<int> at = linesStarting(h.w, "NEEDLE");
    ASSERT_GT(at.size(), 2u);
    EXPECT_EQ(h.matchedLines(), at) << "INV-7: matches differ from the grid";
    const QString counter = h.counterText();
    EXPECT_TRUE(counter.endsWith(QStringLiteral("/") + QString::number(at.size())))
        << "INV-7: the counter " << counter.toStdString();
}

INSTANTIATE_TEST_SUITE_P(SearchMatchesFollowScrollback, OutputDuringASlicedSearch,
                         ::testing::Values(1, 50));
