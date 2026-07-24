// markdownscan — shared CommonMark fence primitives, conformance test.
// See spec.md for the full contract (ANTS-3603).

#include "markdownscan.h"

#include <QChar>
#include <QString>
#include <QStringList>
#include <QVector>

#include <gtest/gtest.h>

using MarkdownScan::fenceMask;
using MarkdownScan::fenceOpenerChar;

// INV-1 — fence opener recognition: 0–3 leading spaces, space-only indent.
TEST(MarkdownScanFence, OpenerCharIndentRule) {
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("```")), QLatin1Char('`'));
    EXPECT_EQ(fenceOpenerChar(QStringLiteral(" ```")), QLatin1Char('`'));
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("  ```")), QLatin1Char('`'));
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("   ```")), QLatin1Char('`'));
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("~~~")), QLatin1Char('~'));
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("```cpp")), QLatin1Char('`'));  // info string
    // 4 leading spaces = indented code, NOT a fence opener.
    EXPECT_TRUE(fenceOpenerChar(QStringLiteral("    ```")).isNull());
    // A tab must not open a fence (ANTS-3598).
    EXPECT_TRUE(fenceOpenerChar(QStringLiteral("\t```")).isNull());
    // Not a fence at all.
    EXPECT_TRUE(fenceOpenerChar(QStringLiteral("plain prose")).isNull());
    EXPECT_TRUE(fenceOpenerChar(QStringLiteral("``")).isNull());  // only two ticks
}

// INV-2 / INV-5 — opener + body + closer masked true; prose false; size matches.
TEST(MarkdownScanFence, MasksFencedBlock) {
    const QStringList lines{
        QStringLiteral("before"),   // 0 false
        QStringLiteral("```cpp"),   // 1 true  (opener)
        QStringLiteral("int x;"),   // 2 true  (body)
        QStringLiteral("```"),      // 3 true  (closer)
        QStringLiteral("after"),    // 4 false
    };
    const QVector<bool> mask = fenceMask(lines);
    ASSERT_EQ(mask.size(), lines.size());
    const QVector<bool> want{false, true, true, true, false};
    EXPECT_EQ(mask, want);
}

// INV-3 — a ``` block is not closed by a ~~~ line; the ~~~ line is body.
TEST(MarkdownScanFence, MismatchedCloserDoesNotClose) {
    const QStringList lines{
        QStringLiteral("```"),      // 0 true  (opener, backtick)
        QStringLiteral("~~~"),      // 1 true  (body — wrong char, does not close)
        QStringLiteral("still in"), // 2 true  (body)
        QStringLiteral("```"),      // 3 true  (matching closer)
        QStringLiteral("out"),      // 4 false
    };
    const QVector<bool> mask = fenceMask(lines);
    const QVector<bool> want{true, true, true, true, false};
    EXPECT_EQ(mask, want);
}

// INV-4 — an unterminated fence masks true to end-of-input.
TEST(MarkdownScanFence, UnterminatedFenceMasksToEnd) {
    const QStringList lines{
        QStringLiteral("prose"),    // 0 false
        QStringLiteral("~~~"),      // 1 true  (opener, never closed)
        QStringLiteral("body a"),   // 2 true
        QStringLiteral("body b"),   // 3 true
    };
    const QVector<bool> mask = fenceMask(lines);
    const QVector<bool> want{false, true, true, true};
    EXPECT_EQ(mask, want);
}

// INV-5 — empty input yields an empty mask (no crash).
TEST(MarkdownScanFence, EmptyInput) {
    EXPECT_TRUE(fenceMask(QStringList{}).isEmpty());
}
