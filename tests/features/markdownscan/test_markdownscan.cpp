// markdownscan — shared CommonMark fence primitives, conformance test.
// See spec.md for the full contract (ANTS-3603).

#include "markdownscan.h"

#include <QChar>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <gtest/gtest.h>

using MarkdownScan::CodeSpan;
using MarkdownScan::codeSpans;
using MarkdownScan::fenceMask;
using MarkdownScan::fenceOpenerChar;
using MarkdownScan::fenceRe;

namespace {

// codeSpans over an unfenced document — the common shape in these fixtures.
QVector<CodeSpan> spansOf(const QStringList &lines) {
    return codeSpans(lines, fenceMask(lines));
}

}  // namespace

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
        // ANTS-3655 — the info-string rule is part of the written statement
        // too, so fenceRe() must reject these alongside the hand-scan.
        QStringLiteral("```` ``` ````"), QStringLiteral("````"),
        QStringLiteral("~~~ `x` "),
    };
    for (const QString &l : lines) {
        const auto m = fenceRe().match(l);
        const QChar got = fenceOpenerChar(l);
        EXPECT_EQ(m.hasMatch(), !got.isNull()) << l.toStdString();
        if (m.hasMatch()) {
            EXPECT_EQ(got, m.captured(1).at(0)) << l.toStdString();
        }
    }
}

// INV-8 (ANTS-3655) — a BACKTICK fence's info string may not contain a
// backtick (CommonMark § 4.5), so a line that is really a multi-backtick
// inline code span opens no fence. Pre-fix the rule stopped at
// `^ {0,3}(```|~~~)`, so ```` ```` ``` ```` ```` read as an opener that never
// closed and masked the rest of the document true.
TEST(MarkdownScanFence, BacktickInfoStringMayNotContainABacktick) {
    // A 4-backtick inline span quoting a 3-backtick run — a paragraph.
    EXPECT_TRUE(fenceOpenerChar(QStringLiteral("```` ``` ````")).isNull());
    EXPECT_TRUE(fenceOpenerChar(QStringLiteral("``` `x` ```")).isNull());
    // A bare run of any length ≥ 3 is still an opener, info string or not.
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("````")), QLatin1Char('`'));
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("```cpp")), QLatin1Char('`'));
    // Tilde fences are unaffected: their info string may contain backticks.
    EXPECT_EQ(fenceOpenerChar(QStringLiteral("~~~ `x` ")), QLatin1Char('~'));
}

// INV-8 — the document-level consequence, which is what made this load-bearing:
// a spec that documents fence syntax by example must stay readable past the
// example. docs/specs/ANTS-3653.md writes exactly this line.
TEST(MarkdownScanFence, FourBacktickSpanLeavesTheRestOfTheDocVisible) {
    const QStringList lines{
        QStringLiteral("prose"),
        QStringLiteral("  ```` ``` ```` at line 4 with a citation at line 9"),
        QStringLiteral("# heading"),
        QStringLiteral("more prose"),
    };
    const QVector<bool> want{false, false, false, false};
    EXPECT_EQ(fenceMask(lines), want);
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

// INV-9 (ANTS-3649) — the fenceMask overload reports an unterminated opener's
// 1-based line, and -1 when the document's fences all close. The fact is not
// recoverable from the mask: a doc ending in a closed block and a doc ending
// inside an unclosed one both end in a run of `true`.
TEST(MarkdownScanFence, UnterminatedOpenerLineReported) {
    int opener = 0;
    const QStringList unclosed{
        QStringLiteral("prose"),
        QStringLiteral("```cpp"),   // line 2, never closed
        QStringLiteral("body"),
    };
    EXPECT_EQ(fenceMask(unclosed, &opener), (QVector<bool>{false, true, true}));
    EXPECT_EQ(opener, 2);

    // Closed — including the case that makes a mask-derived rule wrong: the
    // closer is the document's FINAL line, so the mask still ends in `true`.
    const QStringList closed{
        QStringLiteral("prose"),
        QStringLiteral("```cpp"),
        QStringLiteral("body"),
        QStringLiteral("```"),
    };
    opener = 0;
    EXPECT_EQ(fenceMask(closed, &opener),
              (QVector<bool>{false, true, true, true}));
    EXPECT_EQ(opener, -1);

    // Only the OUTERMOST unclosed opener is reported: a ~~~ line inside an
    // open ``` block is body text, not a second opener.
    const QStringList nested{
        QStringLiteral("```"),      // line 1, never closed
        QStringLiteral("~~~"),      // body
        QStringLiteral("body"),
    };
    opener = 0;
    fenceMask(nested, &opener);
    EXPECT_EQ(opener, 1);

    // A null pointer is accepted (the 1-argument overload's behaviour).
    EXPECT_EQ(fenceMask(unclosed, nullptr), fenceMask(unclosed));
    EXPECT_TRUE(fenceMask(QStringList{}, &opener).isEmpty());
    EXPECT_EQ(opener, -1);
}

// INV-10 (ANTS-3649) — codeSpans locates an inline span's CONTENT bounds plus
// its delimiter run length, so the opening run starts at startCol - delimLen
// and the closing run ends at endCol + delimLen. A content-only struct cannot
// express that for a multi-backtick span, and the consumers' anchor windows
// measure from the delimiter columns.
TEST(MarkdownScanSpans, ContentBoundsAndDelimiterLength) {
    const QStringList one{QStringLiteral("a `code` b")};
    //                     0123456789 — ` at 2, content [3,7), ` at 7
    const auto s1 = spansOf(one);
    ASSERT_EQ(s1.size(), 1);
    EXPECT_EQ(s1[0].startLine, 0);
    EXPECT_EQ(s1[0].startCol, 3);
    EXPECT_EQ(s1[0].endLine, 0);
    EXPECT_EQ(s1[0].endCol, 7);
    EXPECT_EQ(s1[0].delimLen, 1);

    // Double-backtick span quoting a single backtick — delimLen 2, and the
    // inner backtick does NOT close it (CommonMark's equal-run rule).
    const QStringList two{QStringLiteral("x ``a`b`` y")};
    //                     0123456789 — `` at 2-3, content [4,7), `` at 7-8
    const auto s2 = spansOf(two);
    ASSERT_EQ(s2.size(), 1);
    EXPECT_EQ(s2[0].startCol, 4);
    EXPECT_EQ(s2[0].endCol, 7);
    EXPECT_EQ(s2[0].delimLen, 2);
    EXPECT_EQ(two.at(0).mid(s2[0].startCol, s2[0].endCol - s2[0].startCol),
              QStringLiteral("a`b"));

    // Two spans on one line are two spans; scanning resumes past the closer.
    const auto s3 = spansOf(QStringList{QStringLiteral("`a` and `b`")});
    ASSERT_EQ(s3.size(), 2);
    EXPECT_EQ(s3[0].startCol, 1);
    EXPECT_EQ(s3[1].startCol, 9);
}

// INV-10 — content is returned VERBATIM. CommonMark's one-space strip is the
// consumer's job: a caller matching an identifier is unaffected by it, while a
// caller testing "fills the span" applies it.
TEST(MarkdownScanSpans, ContentIsVerbatim) {
    const QStringList lines{QStringLiteral("see ` :45 ` here")};
    const auto s = spansOf(lines);
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(lines.at(0).mid(s[0].startCol, s[0].endCol - s[0].startCol),
              QStringLiteral(" :45 "));
}

// INV-11 (ANTS-3649) — the scan is whole-document: a span may cross a newline
// (CommonMark § 6.1), and a per-line pass leaves its tail exposed. This is the
// live docs/specs/ANTS-1150.md:197-198 shape that ANTS-3635(a) fixed.
TEST(MarkdownScanSpans, SpanCrossesANewline) {
    const QStringList lines{
        QStringLiteral("a `x"),     // ` at col 2, content starts col 3
        QStringLiteral("y` b"),     // ` at col 1, content ends col 1
    };
    const auto s = spansOf(lines);
    ASSERT_EQ(s.size(), 1);
    EXPECT_EQ(s[0].startLine, 0);
    EXPECT_EQ(s[0].startCol, 3);
    EXPECT_EQ(s[0].startCol - s[0].delimLen, 2);  // the opening delimiter
    EXPECT_EQ(s[0].endLine, 1);
    EXPECT_EQ(s[0].endCol, 1);
    EXPECT_EQ(s[0].delimLen, 1);
}

// INV-11 — the forward search for a closer stops at a blank line and at a
// fence line: an inline span crosses neither. The rule decides where a span
// ENDS, which is what the consumers' "fills a whole span" branches on, so a
// genuinely boundary-free whole-document scan would change their counts.
TEST(MarkdownScanSpans, ForwardSearchStopsAtBlankAndFenceLines) {
    EXPECT_TRUE(spansOf(QStringList{QStringLiteral("a `x"),
                                    QStringLiteral(""),
                                    QStringLiteral("y` b")}).isEmpty());
    EXPECT_TRUE(spansOf(QStringList{QStringLiteral("a `x"),
                                    QStringLiteral("```"),
                                    QStringLiteral("```"),
                                    QStringLiteral("y` b")}).isEmpty());
}

// INV-11 — an UNMATCHED run is literal text per CommonMark and yields no span,
// so one stray backtick cannot swallow the rest of the document; and spans
// inside a fenced block are not spans at all (the fence mask wins).
TEST(MarkdownScanSpans, UnmatchedRunAndFencedSpans) {
    EXPECT_TRUE(spansOf(QStringList{QStringLiteral("a stray ` backtick")})
                    .isEmpty());
    EXPECT_TRUE(spansOf(QStringList{QStringLiteral("```"),
                                    QStringLiteral("`code`"),
                                    QStringLiteral("```")}).isEmpty());
    // A run of 3 with no equal-length partner: the ``` here is inline text,
    // not a fence (it is mid-line), and nothing closes it.
    EXPECT_TRUE(spansOf(QStringList{QStringLiteral("a ```x` b")}).isEmpty());
}

// ANTS-3740 — headings(): the ATX collector hoisted out of
// ReadRegion::resolveSection when cold_eyes_brief became its second consumer.
// Level, slug and body span are all published to a reviewer, so all three are
// contract.
TEST(MarkdownScanHeadings, LevelsSlugsAndSpans) {
    const QStringList doc{
        QStringLiteral("# Title"),            // 1
        QStringLiteral("prose"),              // 2
        QStringLiteral("## 2. Surface"),      // 3
        QStringLiteral("### 2.1 a_b"),        // 4
        QStringLiteral("body"),               // 5
        QStringLiteral("## 3. Tests"),        // 6
        QStringLiteral("tail"),               // 7
    };
    const auto h = MarkdownScan::headings(doc);
    ASSERT_EQ(h.size(), 4);

    // The H1 owns the whole document; an H2 stops at the next H2 and OWNS its
    // deeper subsections.
    EXPECT_EQ(h[0].level, 1);
    EXPECT_EQ(h[0].line, 1);
    EXPECT_EQ(h[0].endLine, 7);
    EXPECT_EQ(h[1].text, QStringLiteral("2. Surface"));
    EXPECT_EQ(h[1].line, 3);
    EXPECT_EQ(h[1].endLine, 5);
    EXPECT_EQ(h[2].level, 3);
    EXPECT_EQ(h[2].endLine, 5);
    EXPECT_EQ(h[3].line, 6);
    EXPECT_EQ(h[3].endLine, 7);

    // Slug: lowercase, every run of non-alphanumerics to one '-', trimmed.
    // An underscore is a dash here — this is read_region's key, NOT a GitHub
    // anchor (DocIntegrity::gfmSlug keeps the underscore).
    EXPECT_EQ(h[2].slug, QStringLiteral("2-1-a-b"));
    EXPECT_EQ(MarkdownScan::headingSlug(QStringLiteral("4.2 Emission model")),
              QStringLiteral("4-2-emission-model"));
    // Idempotent: the slug of a slug is itself, so a caller may pass either.
    EXPECT_EQ(MarkdownScan::headingSlug(QStringLiteral("4-2-emission-model")),
              QStringLiteral("4-2-emission-model"));
    EXPECT_TRUE(MarkdownScan::headingSlug(QStringLiteral("   ")).isEmpty());
}

// ANTS-3740 / ANTS-3674 — a '#' inside a fenced block is not a heading, and a
// 4-backtick inline span that DEMONSTRATES a fence is not an opener. Both bit
// read_region's hand-rolled tracker; both would now publish phantom sections.
TEST(MarkdownScanHeadings, FenceAwareness) {
    const auto fenced = MarkdownScan::headings(QStringList{
        QStringLiteral("# Real"),
        QStringLiteral("```"),
        QStringLiteral("# Fenced"),
        QStringLiteral("```"),
        QStringLiteral("## Also real"),
    });
    ASSERT_EQ(fenced.size(), 2);
    EXPECT_EQ(fenced[0].text, QStringLiteral("Real"));
    EXPECT_EQ(fenced[1].text, QStringLiteral("Also real"));

    // A 4-backtick inline span is a paragraph, so the heading after it stays
    // visible (the ANTS-3674 defect made every later heading invisible).
    const auto teaches = MarkdownScan::headings(QStringList{
        QStringLiteral("Use ```` ```cpp ```` to open a block."),
        QStringLiteral("## Still found"),
    });
    ASSERT_EQ(teaches.size(), 1);
    EXPECT_EQ(teaches[0].text, QStringLiteral("Still found"));
}

// ANTS-3740 — headingLevel: 1-6 '#' followed by a space or end-of-line. A
// 7-hash run and `#foo` are not headings, so they must not be indexed.
TEST(MarkdownScanHeadings, LevelRule) {
    EXPECT_EQ(MarkdownScan::headingLevel(QStringLiteral("# a")), 1);
    EXPECT_EQ(MarkdownScan::headingLevel(QStringLiteral("######")), 6);
    EXPECT_EQ(MarkdownScan::headingLevel(QStringLiteral("#######")), 0);
    EXPECT_EQ(MarkdownScan::headingLevel(QStringLiteral("#nospace")), 0);
    EXPECT_EQ(MarkdownScan::headingLevel(QStringLiteral("not a heading")), 0);
    EXPECT_EQ(MarkdownScan::headingLevel(QString()), 0);
}
