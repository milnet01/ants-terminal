// ANTS-3654 — the anchor-symbol drift check: does the line a doc cites still
// mention the symbol the doc names beside it?
//
// Contract: docs/specs/ANTS-3654.md § 2 (anchor / needle / match) and § 3
// (INV-13, INV-14). Invariant map: tests/features/doc_citations_anchor/spec.md.
//
// The fixture and response accessors are shared with the parent engine's tests
// via ../doc_citations/fixture.h — a second copy of the canonical-root logic
// would make these pass or fail for reasons unrelated to anchors.

#include "../../_support/expect.h"
#include "../doc_citations/fixture.h"
#include "doccitations.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QJsonObject>
#include <QString>

ANTS_TEST_SCOPE();

namespace {

using namespace doccit_test;

// One INV-13 row. `docLine` is the whole markdown line — anchor span, gap and
// citation span are written literally, because the gap is measured in columns
// on that raw line and a builder that assembled it from parts would hide the
// one thing several rows are pinning.
struct Row {
    const char *name;
    QByteArray  docLine;
    QByteArray  target;        // contents of src/a.cpp
    bool        wantAnchor;    // are anchor_symbol / anchor_found present at all?
    const char *wantSymbol;    // expected anchor_symbol when present
    bool        wantFound;     // expected anchor_found when present
};

// A citation span sits two characters after its anchor span here (" ("), so
// every row is well inside maxAnchorGap unless it says otherwise.
const Row kRows[] = {
    {"match — needle is on the cited line",
     "`parse` (`src/a.cpp:2`) does the work\n", "one\nint parse(int x);\n", true, "parse", true},

    {"no match — needle absent from the cited line",
     "`parse` (`src/a.cpp:1`) does the work\n", "one\nint parse(int x);\n", true, "parse", false},

    {"substring only — `id` must not match `invalid`",
     "`id` (`src/a.cpp:1`) is the key\n", "return invalid;\n", true, "id", false},

    {"A::b — the field carries the span, the search uses the last component",
     "`A::b` (`src/a.cpp:1`) is inherited\n", "void b() {}\n", true, "A::b", true},

    {"a:b — a lone colon is not a separator, so the needle is the whole anchor",
     "`a:b` (`src/a.cpp:1`) is odd\n", "void b() {}\n", true, "a:b", false},

    {"Foo:: — an empty final component is discarded",
     "`Foo::` (`src/a.cpp:1`) trails\n", "struct Foo {};\n", false, "", false},

    {"case differs — Parse must not match parse",
     "`Parse` (`src/a.cpp:1`) is capitalised\n", "int parse(int x);\n", true, "Parse", false},

    {"citation not inside a code span — no anchor, whatever precedes it",
     "`parse` src/a.cpp:2 bare\n", "one\nint parse(int x);\n", false, "", false},

    {"a non-identifier span sits between anchor and citation — no anchor",
     "`parse` `and then` (`src/a.cpp:2`) x\n", "one\nint parse(int x);\n", false, "", false},

    {"` foo ` — content is matched verbatim, no CommonMark one-space strip",
     "` foo ` (`src/a.cpp:1`) spaced\n", "int foo(void);\n", false, "", false},

    {"CRLF target — the trailing \\r must not produce a phantom mismatch",
     "`parse` (`src/a.cpp:1`) crlf\n", "int parse(int x);\r\n", true, "parse", true},

    {"EOF-clamped range — the search covers the portion that exists",
     "`parse` (`src/a.cpp:1-9999`) clamped\n", "int parse(int x);\n", true, "parse", true},

    {"missing_file — a good anchor beside a status that emits no text",
     "`parse` (`src/gone.cpp:1`) absent\n", "int parse(int x);\n", false, "", false},
};

QJsonObject runRow(const Row &r, DocCitations::Options opts = {}) {
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), r.target);
    const QString docPath = fx.doc(r.docLine);
    return DocCitations::check(fx.root, docPath, opts);
}

bool has(const QJsonObject &c, const char *key) {
    return c.contains(QString::fromLatin1(key));
}

// INV-13 — anchor resolution, needle derivation and the match rule.
//
// Every row reports independently: no ASSERT_* in the loop, because a fatal
// assertion returns from the whole TEST and would let one broken row mask every
// row after it. `continue` on a row that cannot be checked further.
TEST(DocCitationsAnchor, Inv13AnchorTable) {
    for (const Row &r : kRows) {
        const QJsonObject res  = runRow(r);
        const std::string dump = render(res).toStdString();

        if (cites(res).size() != 1) {
            ADD_FAILURE() << r.name << " — expected exactly one citation — " << dump;
            continue;
        }
        const QJsonObject c = cite(res, 0);

        if (!r.wantAnchor) {
            EXPECT_FALSE(has(c, "anchor_symbol"))
                << r.name << " — anchor_symbol should be absent — " << dump;
            EXPECT_FALSE(has(c, "anchor_found"))
                << r.name << " — anchor_found should be absent — " << dump;
            continue;
        }

        if (!has(c, "anchor_symbol") || !has(c, "anchor_found")) {
            ADD_FAILURE() << r.name << " — expected both anchor fields — " << dump;
            continue;
        }
        EXPECT_EQ(c.value(QStringLiteral("anchor_symbol")).toString(),
                  QString::fromUtf8(r.wantSymbol))
            << r.name << " — " << dump;
        EXPECT_EQ(c.value(QStringLiteral("anchor_found")).toBool(), r.wantFound)
            << r.name << " — " << dump;
    }
}

// INV-13's boundary row, as a PAIR: either side alone passes under both
// `gap < maxAnchorGap` and `gap <= maxAnchorGap`, so only the pair pins it.
TEST(DocCitationsAnchor, Inv13GapBoundaryIsInclusive) {
    // Gap is measured from one past the anchor's closing delimiter to the
    // citation's opening one, so N spaces between the spans is a gap of N.
    const auto rowWithGap = [](int gap) {
        Row r{"gap", QByteArray(), "int parse(int x);\n", true, "parse", true};
        r.docLine = "`parse`" + QByteArray(gap, ' ') + "(`src/a.cpp:1`)\n";
        return r;
    };

    DocCitations::Options opts;
    opts.maxAnchorGap = 8;

    // A gap of exactly maxAnchorGap: the "(" plus 7 spaces.
    const QJsonObject atLimit = runRow(rowWithGap(7), opts);
    ASSERT_EQ(cites(atLimit).size(), 1) << render(atLimit).toStdString();
    EXPECT_TRUE(has(cite(atLimit, 0), "anchor_symbol"))
        << "gap == maxAnchorGap must still find the anchor — "
        << render(atLimit).toStdString();

    // One past it.
    const QJsonObject overLimit = runRow(rowWithGap(8), opts);
    ASSERT_EQ(cites(overLimit).size(), 1) << render(overLimit).toStdString();
    EXPECT_FALSE(has(cite(overLimit, 0), "anchor_symbol"))
        << "gap == maxAnchorGap + 1 must find no anchor — "
        << render(overLimit).toStdString();
}

// INV-13's last row: the verdict comes from the full resolved range, while the
// RESPONSE stays clipped. This is the row that catches a widened read leaking
// into `text` — the regression `range_truncated` cannot see, because it is
// derived from the citation's own line numbers rather than from what was read.
TEST(DocCitationsAnchor, Inv13FullRangeVerdictLeavesTextClipped) {
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"),
             "1\n2\n3\n4\n5\n6\n7\nint parse(int x);\n9\n10\n");
    const QString docPath = fx.doc("`parse` (`src/a.cpp:1-10`) spans\n");

    DocCitations::Options opts;
    opts.maxRangeLines = 3;
    const QJsonObject res = DocCitations::check(fx.root, docPath, opts);

    ASSERT_EQ(cites(res).size(), 1) << render(res).toStdString();
    const QJsonObject c = cite(res, 0);

    EXPECT_TRUE(c.value(QStringLiteral("anchor_found")).toBool())
        << "the needle is on line 8, inside the cited range — "
        << render(res).toStdString();
    EXPECT_EQ(textOf(c).size(), 3)
        << "text must stay clipped to max_range_lines — " << render(res).toStdString();
    EXPECT_TRUE(c.value(QStringLiteral("range_truncated")).toBool())
        << render(res).toStdString();
}

// INV-14 — `only:"stale"` selects on the anchor verdict as well as on status,
// while the tallies stay whole-doc and `unparsed[]` is never filtered.
TEST(DocCitationsAnchor, Inv14StaleKeepsAnchorMissing) {
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "int parse(int x);\n");
    fx.write(QStringLiteral("src/b.cpp"), "int other(int x);\n");

    // 3 anchored-and-found, 1 anchored-but-missing, 2 unanchored ok,
    // 1 missing_file, 1 unparsed.
    const QString docPath = fx.doc(
        "`parse` (`src/a.cpp:1`) one\n"
        "`parse` (`src/a.cpp:1`) two\n"
        "`parse` (`src/a.cpp:1`) three\n"
        "`parse` (`src/b.cpp:1`) drifted\n"
        "bare src/a.cpp:1\n"
        "bare src/b.cpp:1\n"
        "`parse` (`src/gone.cpp:1`) absent\n"
        "prose `6.2:1`\n");

    DocCitations::Options opts;
    opts.only = DocCitations::Only::Stale;
    const QJsonObject res = DocCitations::check(fx.root, docPath, opts);
    const QJsonObject counts = res.value(QStringLiteral("counts")).toObject();
    const QString dump = render(res);

    EXPECT_EQ(res.value(QStringLiteral("returned")).toInt(), 2)
        << "1 missing_file + 1 anchor-missing ok — " << dump.toStdString();
    EXPECT_EQ(res.value(QStringLiteral("count")).toInt(), 7) << dump.toStdString();
    EXPECT_EQ(counts.value(QStringLiteral("ok")).toInt(), 6) << dump.toStdString();
    EXPECT_EQ(counts.value(QStringLiteral("anchor_missing")).toInt(), 1) << dump.toStdString();
    EXPECT_EQ(counts.value(QStringLiteral("unchecked")).toInt(), 2)
        << "unchecked narrows to the ok citations with no anchor — " << dump.toStdString();
    EXPECT_EQ(counts.value(QStringLiteral("unparsed")).toInt(), 1) << dump.toStdString();
    EXPECT_EQ(res.value(QStringLiteral("unparsed_total")).toInt(), 1) << dump.toStdString();

    // The array, not just the count: "never filters unparsed[]" is about the
    // array, and the count alone would pass an implementation that filtered it.
    EXPECT_EQ(unparsed(res).size(), 1) << dump.toStdString();
}

}  // namespace
