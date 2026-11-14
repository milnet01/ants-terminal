// Search matches follow their lines when scrollback drops its oldest.
// Contract: tests/features/search_matches_follow_scrollback/spec.md.

#include <gtest/gtest.h>

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
