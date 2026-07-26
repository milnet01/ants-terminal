// markdownscan — shared CommonMark fence primitives, conformance test.
// See spec.md for the full contract (ANTS-3603).

#include "markdownscan.h"

#include <QChar>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <gtest/gtest.h>

using MarkdownScan::fenceMask;
using MarkdownScan::fenceOpenerChar;
using MarkdownScan::fenceRe;

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

// INV-6 (ANTS-3638) — fenceOpenerChar hand-scans rather than matching
// fenceRe(), because its indent limit varies. fenceRe() is still the written
// statement of the top-level rule, so pin the two together: at the default
// limit they must agree on every line, or one can drift without the other.
TEST(MarkdownScanFence, HandScanAgreesWithFenceReAtDefaultIndent) {
    const QStringList lines{
        QStringLiteral("```"),        QStringLiteral(" ~~~lua"),
        QStringLiteral("   ```cpp"),  QStringLiteral("    ```"),
        QStringLiteral("\t~~~"),      QStringLiteral("``"),
        QStringLiteral("~~"),         QStringLiteral(""),
        QStringLiteral("prose ```"),  QStringLiteral("  \\```cpp"),
    };
    for (const QString &l : lines) {
        const auto m = fenceRe().match(l);
        const QChar got = fenceOpenerChar(l);
        EXPECT_EQ(m.hasMatch(), !got.isNull()) << l.toStdString();
        if (m.hasMatch()) EXPECT_EQ(got, m.captured(1).at(0)) << l.toStdString();
    }
}

// INV-7 (ANTS-3638) — a fence nested in a list item opens. CommonMark
// re-bases the item's content at the marker's content column, so the 4-space
// indent here is 2 past the column and still within the 3-space allowance.
// Pre-fix the fence never opened and `[x](gone.md)` was scanned as prose.
TEST(MarkdownScanFence, ListNestedFenceOpens) {
    const QStringList lines{
        QStringLiteral("- item text"),        // 0 false
        QStringLiteral(""),                   // 1 false
        QStringLiteral("    ```cpp"),         // 2 true  (opener, col 2 + 2)
        QStringLiteral("    [x](gone.md)"),   // 3 true  (body — sample, not a link)
        QStringLiteral("    ```"),            // 4 true  (closer)
        QStringLiteral(""),                   // 5 false
        QStringLiteral("after"),              // 6 false
    };
    const QVector<bool> want{false, false, true, true, true, false, false};
    EXPECT_EQ(fenceMask(lines), want);
}

// INV-7 converse — leaving the list restores the top-level 3-space limit, so
// an indented block after the list is NOT read as a fence. Without this the
// widening would be unbounded and could swallow the rest of a document.
TEST(MarkdownScanFence, IndentAllowanceResetsAfterTheList) {
    const QStringList lines{
        QStringLiteral("  - nested item"),  // 0 false (content col 4)
        QStringLiteral("      ```"),        // 1 true  (opener, col 4 + 2)
        QStringLiteral("      body"),       // 2 true
        QStringLiteral("      ```"),        // 3 true  (closer)
        QStringLiteral("top-level prose"),  // 4 false — pops the container
        QStringLiteral("    ```"),          // 5 false — 4 spaces, no container
        QStringLiteral("still prose"),      // 6 false
    };
    const QVector<bool> want{false, true, true, true, false, false, false};
    EXPECT_EQ(fenceMask(lines), want);
}
