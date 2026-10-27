// A span found in a line's text lands on the cells that hold it.
// Contract: tests/features/span_cell_columns/spec.md.

#include <gtest/gtest.h>

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "terminalgrid.h"
#include "terminalwidget.h"
#include "vtparser.h"

namespace {

using Cols = std::pair<int, int>;

struct Harness {
    TerminalWidget w;
    VtParser parser{[this](const VtAction &a) { w.grid()->processAction(a); }};
    QLineEdit *input = nullptr;
    QPushButton *regexBtn = nullptr;

    Harness() {
        w.grid()->resize(6, 40);
        QWidget *bar = w.findChild<QWidget *>(QStringLiteral("searchBar"));
        if (!bar) return;
        input = bar->findChild<QLineEdit *>();
        for (QPushButton *b : bar->findChildren<QPushButton *>())
            if (b->text() == QStringLiteral(".*")) regexBtn = b;
    }
    bool ready() const { return input && regexBtn; }
    void feed(const std::string &s) { parser.feed(s.data(), static_cast<int>(s.size())); }

    // Regex mode ON scans synchronously.
    void search(const QString &pattern) {
        input->setText(pattern);
        if (regexBtn->isChecked()) regexBtn->toggle();
        regexBtn->toggle();
    }

    int globalLine(int screenRow) { return w.grid()->scrollbackSize() + screenRow; }

    // The cells of `row` whose codepoints spell the ASCII `needle`, from the
    // grid itself. {-1,-1} when absent.
    Cols cellsOf(int row, const std::string &needle) {
        const TerminalGrid *g = w.grid();
        const int n = static_cast<int>(needle.size());
        for (int c = 0; c + n <= g->cols(); ++c) {
            bool hit = true;
            for (int k = 0; k < n && hit; ++k)
                hit = g->cellAt(row, c + k).codepoint == static_cast<unsigned char>(needle[k]);
            if (hit) return {c, c + n - 1};
        }
        return {-1, -1};
    }
};

bool contains(const std::vector<Cols> &v, Cols c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

}  // namespace

TEST(SpanCellColumns, UrlAfterEmoji) {
    Harness h;
    h.feed("\xF0\x9F\x98\x80 see https://a.example/x\r\n");   // U+1F600
    const Cols want = h.cellsOf(0, "https://a.example/x");
    ASSERT_GE(want.first, 0) << "URL not on the grid";
    const auto spans = h.w.urlSpanColumnsForTest(h.globalLine(0));
    EXPECT_TRUE(contains(spans, want))
        << "INV-1: URL span not at cells " << want.first << ".." << want.second;
}

TEST(SpanCellColumns, UrlAfterOneCellNonBmp) {
    Harness h;
    h.feed("x\xF0\x9D\x90\x80 https://b.example/y\r\n");       // U+1D400
    const Cols want = h.cellsOf(0, "https://b.example/y");
    ASSERT_GE(want.first, 0) << "URL not on the grid";
    const auto spans = h.w.urlSpanColumnsForTest(h.globalLine(0));
    EXPECT_TRUE(contains(spans, want))
        << "INV-2: URL span not at cells " << want.first << ".." << want.second;
}

TEST(SpanCellColumns, SearchMatchAfterCombiningMark) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.feed("cafe\xCC\x81 NEEDLE\r\n");                          // e + U+0301
    ASSERT_FALSE(h.w.grid()->screenCombining(0).empty()) << "no combining mark stored";
    const Cols want = h.cellsOf(0, "NEEDLE");
    ASSERT_GE(want.first, 0);
    h.search(QStringLiteral("NEEDLE"));
    const auto matches = h.w.searchMatchColumnsForTest(h.globalLine(0));
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front(), want) << "INV-3";
}

TEST(SpanCellColumns, SearchMatchEndingOnWideCharacter) {
    Harness h;
    ASSERT_TRUE(h.ready()) << "search bar widgets not found";
    h.feed("a\xE4\xB8\x96" "b\r\n");                            // a, U+4E16 (wide), b
    ASSERT_TRUE(h.w.grid()->cellAt(0, 1).isWideChar) << "U+4E16 not stored wide";
    h.search(QStringLiteral("a\\x{4E16}"));
    const auto matches = h.w.searchMatchColumnsForTest(h.globalLine(0));
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front(), Cols(0, 2)) << "INV-4";
}
