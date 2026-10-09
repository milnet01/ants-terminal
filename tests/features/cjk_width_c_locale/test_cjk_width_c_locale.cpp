// Feature-conformance test for spec.md (ANTS-3792) — CJK and combining
// characters keep their width when the process starts under LC_ALL=C.

#include "terminalgrid.h"
#include "vtparser.h"
#include "_support/srcgrep.h"

#include <clocale>
#include <gtest/gtest.h>
#include <string>

namespace {

struct Harness {
    TerminalGrid grid;
    VtParser parser;
    Harness(int rows, int cols)
        : grid(rows, cols),
          parser([this](const VtAction &a) { grid.processAction(a); }) {}
    void feed(const std::string &s) {
        parser.feed(s.data(), static_cast<int>(s.size()));
    }
};

// Start from the C locale, as a user launching under LC_ALL=C does, and
// put the ambient locale back afterwards for the rest of the bundle.
class CjkWidthCLocale : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(std::setlocale(LC_ALL, "C"), nullptr);
        ok = TerminalGrid::ensureUtf8CType();
    }
    void TearDown() override { std::setlocale(LC_ALL, ""); }
    bool ok = false;
};

TEST_F(CjkWidthCLocale, WideCharIsTwoCells) {
    EXPECT_TRUE(ok) << "ensureUtf8CType found no UTF-8 locale";
    Harness h(5, 20);
    h.feed("\xe4\xb8\x80");  // U+4E00
    EXPECT_TRUE(h.grid.cellAt(0, 0).isWideChar) << "INV-1: U+4E00 not wide";
    EXPECT_EQ(h.grid.cursorCol(), 2) << "INV-1: cursor did not advance two";
}

TEST_F(CjkWidthCLocale, CombiningMarkJoinsPreviousCell) {
    Harness h(5, 20);
    h.feed("e\xcc\x81");  // e + U+0301
    EXPECT_EQ(h.grid.cursorCol(), 1) << "INV-2: combining mark took a cell";
    const auto &comb = h.grid.screenCombining(0);
    auto it = comb.find(0);
    ASSERT_NE(it, comb.end()) << "INV-2: no combining mark on cell 0";
    EXPECT_EQ(it->second.front(), 0x301u);
}

TEST(CjkWidthCLocaleSource, MainCallsItAfterQApplication) {
    const std::string src = ants_test::slurpFile(SRC_MAIN_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "cannot read " << SRC_MAIN_CPP_PATH;
    const auto app = src.find("QApplication app(");
    const auto call = src.find("TerminalGrid::ensureUtf8CType()");
    ASSERT_NE(app, std::string::npos);
    ASSERT_NE(call, std::string::npos) << "INV-3: main.cpp never calls it";
    EXPECT_GT(call, app) << "INV-3: called before QApplication resets the locale";
}

}  // namespace
