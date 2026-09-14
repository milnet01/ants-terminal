// Feature-conformance test for ANTS-4820 — the CommonMark § 4.5 closer-length
// rule reaches the five files that hand-roll their own fence loop instead of
// calling fenceMask. INV-1/2 drive the shared predicate; INV-3/4/5 drive a
// consumer end to end, because a predicate all five call is only worth as
// much as the sites actually calling it. See spec.md.

#include "markdownscan.h"
#include "fileoutline.h"
#include "speclog.h"
#include "feedbackfile.h"

#include "../../_support/expect.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

const QChar kTick = QLatin1Char('`');

// A document whose ONLY `## Heading` is written inside a four-backtick block,
// quoted there together with a three-backtick line. That inner short run is
// what used to end the block early and promote the heading to a real one.
QString docQuotingFenceSyntax(const QString &heading) {
    return QStringLiteral(
        "# Real Title\n"
        "Prose.\n"
        "\n"
        "````\n"          // opener: run of 4
        "```\n"           // sample text — NOT a closer
        "%1\n"            // sample text — NOT a heading
        "````\n"          // the real closer
        "\n"
        "Trailing prose.\n").arg(heading);
}

}  // namespace

// INV-1 — the shared predicate's three arms.
TEST(FenceCloserRunConsumers, Inv1PredicateHonoursRunLength) {
    expect_reset();
    using MarkdownScan::fenceCloses;
    expect(!fenceCloses(QStringLiteral("```"), kTick, 4),
           "INV-1: a shorter run does not close");
    expect(fenceCloses(QStringLiteral("````"), kTick, 4),
           "INV-1: an equal-length run closes");
    expect(fenceCloses(QStringLiteral("`````"), kTick, 4),
           "INV-1: a longer run closes");
    expect(!fenceCloses(QStringLiteral("~~~~"), kTick, 4),
           "INV-1: a different fence character does not close");
    EXPECT_EQ(0, expect_finish());
}

// INV-2 — a null opener answers false, so a site may ask before testing
// whether it is inside a fence at all.
TEST(FenceCloserRunConsumers, Inv2NullOpenerIsFalse) {
    expect_reset();
    expect(!MarkdownScan::fenceCloses(QStringLiteral("````"), QChar(), 0),
           "INV-2: a null open character closes nothing");
    EXPECT_EQ(0, expect_finish());
}

// INV-3 — fileoutline.cpp. Its comment stated the rule WITHOUT the length
// half, so the code matched its own description and was still wrong.
TEST(FenceCloserRunConsumers, Inv3OutlineSkipsAHeadingInsideALongerFence) {
    expect_reset();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("d.md"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(docQuotingFenceSyntax(QStringLiteral("## Not A Real Heading"))
                .toUtf8());
    f.close();

    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Md, /*includeDocComment=*/false,
        /*maxSymbols=*/50);
    bool sawPhantom = false, sawReal = false;
    for (const auto v : out.value(QStringLiteral("symbols")).toArray()) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (n.contains(QStringLiteral("Not A Real Heading"))) sawPhantom = true;
        if (n.contains(QStringLiteral("Real Title")))         sawReal = true;
    }
    expect(sawReal, "INV-3 precondition: the outline found the real heading");
    expect(!sawPhantom,
           "INV-3: a heading inside a longer fence is not outlined");
    EXPECT_EQ(0, expect_finish());
}

// INV-4 — speclog.cpp. Its section finders decide WHERE an appended row
// lands, so a phantom heading does not merely mislead a reader: the write
// goes into somebody's code sample.
//
// The observable is whether the section was FOUND or CREATED. Asserting the
// block came back unchanged is not enough and was the first version of this
// test: appendLoop appends at the END of the section it finds, and a phantom
// heading's section runs to EOF, so the append lands past the block either
// way and the assertion passed under the defect. A created section adds a
// second heading; a found one does not.
TEST(FenceCloserRunConsumers, Inv4SpecLogSkipsASectionInsideALongerFence) {
    expect_reset();
    const QString heading = QStringLiteral("## Cold-eyes loop log");
    const SpecLog::EditResult r =
        SpecLog::appendLoop(docQuotingFenceSyntax(heading),
                            QStringLiteral("loop 1"), QStringLiteral("body"));
    expect(r.ok, "INV-4 precondition: the append succeeded", r.error);
    if (r.ok) {
        expect(r.content.count(heading) == 2,
               "INV-4: the quoted heading was not treated as a real section, "
               "so a real one was created alongside it");
        const QStringList out = r.content.split(QLatin1Char('\n'));
        const int opener = out.indexOf(QStringLiteral("````"));
        expect(opener >= 0 && out.value(opener + 2) == heading,
               "INV-4: the sample heading is still inside the block");
    }
    EXPECT_EQ(0, expect_finish());
}

// INV-5 — feedbackfile.cpp. Its boundary scanner decides where the untriaged
// delta starts, so a phantom `## ` boundary inside a sample changes what a
// contributor's next feedback_query returns.
//
// The fixture needs a maintainer heading BEFORE the block: the delta begins at
// the first contributor heading AFTER the last maintainer one, so without it
// the delta starts at line 1 whatever the scanner thinks of the phantom, and
// the assertion measures nothing. That was the first version of this test and
// it passed under the defect.
TEST(FenceCloserRunConsumers, Inv5FeedbackBoundarySkipsALongerFence) {
    expect_reset();
    const QString doc = QString::fromUtf8(
        "# Real Title\n"                                            // 1
        "\n"                                                        // 2
        "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking (2026-01-01)\n"  // 3
        "Maintainer body.\n"                                        // 4
        "\n"                                                        // 5
        "````\n"                                                    // 6  opener, run 4
        "```\n"                                                     // 7  sample
        "## Phantom Boundary\n"                                     // 8  sample
        "````\n"                                                    // 9  real closer
        "\n"                                                        // 10
        "## Real Contributor Heading\n"                             // 11
        "Contributor body.\n");                                     // 12
    const int phantomLine = 8, realLine = 11;

    const QStringList src = doc.split(QLatin1Char('\n'));
    expect(src.value(phantomLine - 1) == QStringLiteral("## Phantom Boundary")
               && src.value(realLine - 1)
                      == QStringLiteral("## Real Contributor Heading"),
           "INV-5 precondition: the two headings sit on the named lines");

    const FeedbackFile::ParseResult r = FeedbackFile::parse(doc);
    expect(r.deltaStartLine != phantomLine,
           "INV-5: the delta does not start at a heading inside a code sample");
    expect(r.deltaStartLine == realLine,
           "INV-5: the delta starts at the first heading outside the block");
    EXPECT_EQ(0, expect_finish());
}

namespace {

// ANTS-4987 — a document whose only `## Heading` follows an info-string line
// inside an open three-backtick block. That line used to close the block, so
// the heading after it was read as real and the bare closer below it opened a
// fresh block that ran to the end of the file.
QString docWithInfoStringInsideFence(const QString &heading) {
    return QStringLiteral(
        "# Real Title\n"
        "Prose.\n"
        "\n"
        "```\n"          // opener: run of 3, no info string
        "```json\n"      // sample text — NOT a closer, it carries an info string
        "%1\n"           // sample text — NOT a heading
        "```\n"          // the real closer
        "\n"
        "Trailing prose.\n").arg(heading);
}

}  // namespace

// INV-6 — a closer carries no info string; whitespace after the run, a CRLF's
// \r included, still closes.
TEST(FenceCloserRunConsumers, Inv6CloserCarriesNoInfoString) {
    expect_reset();
    using MarkdownScan::fenceCloses;
    expect(!fenceCloses(QStringLiteral("```json"), kTick, 3),
           "INV-6: a run followed by an info string does not close");
    expect(!fenceCloses(QStringLiteral("~~~ text"), QLatin1Char('~'), 3),
           "INV-6: the rule holds for a tilde fence too");
    expect(fenceCloses(QStringLiteral("```   "), kTick, 3),
           "INV-6: trailing spaces still close");
    expect(fenceCloses(QStringLiteral("```\r"), kTick, 3),
           "INV-6: a CRLF closer still closes");
    EXPECT_EQ(0, expect_finish());
}

// INV-7 — fenceMask shares the rule, so the two families cannot disagree
// about the same file.
TEST(FenceCloserRunConsumers, Inv7MaskKeepsTheBlockOpenAcrossAnInfoString) {
    expect_reset();
    const QStringList lines =
        docWithInfoStringInsideFence(QStringLiteral("## Phantom"))
            .split(QLatin1Char('\n'));
    const QVector<bool> mask = MarkdownScan::fenceMask(lines);
    const int phantom  = lines.indexOf(QStringLiteral("## Phantom"));
    const int trailing = lines.indexOf(QStringLiteral("Trailing prose."));
    expect(phantom > 0 && mask.value(phantom),
           "INV-7: the heading after an info-string line stays inside the block");
    expect(trailing > 0 && !mask.value(trailing),
           "INV-7: the real closer still ends the block");
    EXPECT_EQ(0, expect_finish());
}

// INV-8 — fileoutline.cpp, the INV-3 surface.
TEST(FenceCloserRunConsumers, Inv8OutlineSkipsAHeadingAfterAnInfoStringLine) {
    expect_reset();
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("d.md"));
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(docWithInfoStringInsideFence(QStringLiteral("## Not A Real Heading"))
                .toUtf8());
    f.close();

    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Md, /*includeDocComment=*/false,
        /*maxSymbols=*/50);
    bool sawPhantom = false, sawReal = false;
    for (const auto v : out.value(QStringLiteral("symbols")).toArray()) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (n.contains(QStringLiteral("Not A Real Heading"))) sawPhantom = true;
        if (n.contains(QStringLiteral("Real Title")))         sawReal = true;
    }
    expect(sawReal, "INV-8 precondition: the outline found the real heading");
    expect(!sawPhantom,
           "INV-8: a heading after an info-string line inside a fence is not "
           "outlined");
    EXPECT_EQ(0, expect_finish());
}

// INV-9 — speclog.cpp, the INV-4 surface. Found versus created, for the reason
// INV-4 records.
TEST(FenceCloserRunConsumers, Inv9SpecLogSkipsASectionAfterAnInfoStringLine) {
    expect_reset();
    const QString heading = QStringLiteral("## Cold-eyes loop log");
    const SpecLog::EditResult r =
        SpecLog::appendLoop(docWithInfoStringInsideFence(heading),
                            QStringLiteral("loop 1"), QStringLiteral("body"));
    expect(r.ok, "INV-9 precondition: the append succeeded", r.error);
    if (r.ok) {
        expect(r.content.count(heading) == 2,
               "INV-9: the quoted heading was not treated as a real section, "
               "so a real one was created alongside it");
        const QStringList out = r.content.split(QLatin1Char('\n'));
        const int info = out.indexOf(QStringLiteral("```json"));
        expect(info >= 0 && out.value(info + 1) == heading,
               "INV-9: the sample heading is still inside the block");
    }
    EXPECT_EQ(0, expect_finish());
}

// INV-10 — feedbackfile.cpp, the INV-5 surface. The maintainer heading before
// the block is load-bearing for the reason INV-5 records.
TEST(FenceCloserRunConsumers, Inv10FeedbackBoundarySkipsAnInfoStringLine) {
    expect_reset();
    const QString doc = QString::fromUtf8(
        "# Real Title\n"                                            // 1
        "\n"                                                        // 2
        "## \xF0\x9F\x93\x8B Ants Terminal roadmap tracking (2026-01-01)\n"  // 3
        "Maintainer body.\n"                                        // 4
        "\n"                                                        // 5
        "```\n"                                                     // 6  opener
        "```json\n"                                                 // 7  sample
        "## Phantom Boundary\n"                                     // 8  sample
        "```\n"                                                     // 9  real closer
        "\n"                                                        // 10
        "## Real Contributor Heading\n"                             // 11
        "Contributor body.\n");                                     // 12
    const int phantomLine = 8, realLine = 11;

    const QStringList src = doc.split(QLatin1Char('\n'));
    expect(src.value(phantomLine - 1) == QStringLiteral("## Phantom Boundary")
               && src.value(realLine - 1)
                      == QStringLiteral("## Real Contributor Heading"),
           "INV-10 precondition: the two headings sit on the named lines");

    const FeedbackFile::ParseResult r = FeedbackFile::parse(doc);
    expect(r.deltaStartLine != phantomLine,
           "INV-10: the delta does not start at a heading inside a code sample");
    expect(r.deltaStartLine == realLine,
           "INV-10: the delta starts at the first heading outside the block");
    EXPECT_EQ(0, expect_finish());
}
