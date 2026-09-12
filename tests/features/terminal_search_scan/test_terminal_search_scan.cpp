// ANTS-2000 — terminal search results are fixed by the grid, not by how the
// scan reads it. Contract: tests/features/terminal_search_scan/spec.md.

#include <gtest/gtest.h>

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
    EXPECT_EQ(h.count(QStringLiteral("short +$")), 1) << "INV-5";
}
