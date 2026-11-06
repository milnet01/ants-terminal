// A paste cannot end bracketed-paste mode early — see spec.md. ANTS-6442.

#include "bracketedpaste.h"

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <string>

#ifndef SRC_TERMINALWIDGET_IMPL_PATH
#  error "SRC_TERMINALWIDGET_IMPL_PATH compile definition required"
#endif

namespace {

const QByteArray kMarkers[] = {
    QByteArray("\x1B[200~"), QByteArray("\x1B[201~"),
    QByteArray("\xC2\x9B" "200~"), QByteArray("\xC2\x9B" "201~"),
    QByteArray("\x9B" "200~"), QByteArray("\x9B" "201~"),
};

bool holdsMarker(const QByteArray &s) {
    for (const QByteArray &m : kMarkers)
        if (s.contains(m)) return true;
    return false;
}

}  // namespace

TEST(BracketedPasteSanitize, Inv1PlainMarkersRemoved) {
    for (const QByteArray &m : kMarkers) {
        const QByteArray out = stripBracketedPasteMarkers("a" + m + "b");
        EXPECT_EQ(out, QByteArray("ab")) << m.toHex().constData();
    }
}

TEST(BracketedPasteSanitize, Inv2NestedMarkersRemoved) {
    const QByteArray cases[] = {
        // Removing the inner end marker joins ESC[20 + 0~ into ESC[200~.
        QByteArray("\x1B[20") + "\x1B[201~" + "0~rm -rf ~\r",
        QByteArray("\x1B[20") + "\x1B[200~" + "1~id\r",
        // Three deep.
        QByteArray("\x1B[2") + "\x1B[20" + "\x1B[201~" + "1~" + "01~",
        // 8-bit outside, 7-bit inside, and the reverse.
        QByteArray("\x9B" "20") + "\x1B[201~" + "1~",
        QByteArray("\x1B[20") + "\x9B" "201~" + "1~",
        QByteArray("\xC2\x9B" "20") + "\xC2\x9B" "200~" + "1~",
    };
    for (const QByteArray &c : cases) {
        const QByteArray out = stripBracketedPasteMarkers(c);
        EXPECT_FALSE(holdsMarker(out)) << "in " << c.toHex().constData()
                                       << " out " << out.toHex().constData();
    }
}

TEST(BracketedPasteSanitize, Inv3OtherTextUnchanged) {
    const QByteArray plain("echo hi\n\x1B[31mred\x1B[0m \x1B[20~ \x9B" "2J ü");
    EXPECT_EQ(stripBracketedPasteMarkers(plain), plain);
}

TEST(BracketedPasteSanitize, Inv4PerformPasteWrapsTheStrippedPaste) {
    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALWIDGET_IMPL_PATH, "void TerminalWidget::performPaste("));
    ASSERT_FALSE(body.empty()) << "performPaste body not found";
    EXPECT_NE(body.find("stripBracketedPasteMarkers(data)"), std::string::npos);
}
