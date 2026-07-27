// ANTS-3659 — the `doc-examples` suppression region. Scan-layer rows are
// string literals (no filesystem, per ANTS-3653 § 5); the two check-layer rows
// need a temp dir, because antecedent inheritance and the JSON envelope belong
// to the read path. See spec.md for the invariant map and
// docs/specs/ANTS-3659.md for the contract.

#include "doccitations.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include "../../_support/expect.h"
#include "../doc_citations/fixture.h"
ANTS_TEST_SCOPE();

using DocCitations::Citation;
using DocCitations::Options;
using DocCitations::ScanResult;
using DocCitations::Unparsed;
using doccit_test::Fixture;

namespace {

const char *const kBegin = "<!-- doc-examples: begin -->";
const char *const kEnd   = "<!-- doc-examples: end -->";

ScanResult scan(const QString &text, const Options &opts = {}) {
    return DocCitations::scan(text.split(QLatin1Char('\n')), opts);
}

// Build a document from explicit lines, so a fixture's line numbers are read
// off the source rather than counted out of a blob.
QString lines(std::initializer_list<const char *> ls) {
    QStringList out;
    for (const char *l : ls) out << QString::fromUtf8(l);
    return out.join(QLatin1Char('\n'));
}

QString render(const ScanResult &r) {
    QString s = QStringLiteral("sup=%1 unterm=%2 | ")
                    .arg(r.examplesSuppressed).arg(r.unterminatedExamples);
    for (const Citation &c : r.citations)
        s += QStringLiteral("[%1 %2 %3]")
                 .arg(c.docLine)
                 .arg(c.path.isEmpty() ? QStringLiteral("<cont>") : c.path)
                 .arg(c.startLine);
    for (const Unparsed &u : r.unparsed)
        s += QStringLiteral("{%1 %2 %3}").arg(u.docLine).arg(u.raw).arg(u.reason);
    return s;
}

}  // namespace

// INV-49 + INV-52 — the mask drops entries from BOTH arrays, and
// examplesSuppressed counts both. The in-region pair is one citation and one
// `bad_locus` token: a merely-unresolvable path like `6.2:1` is a CITATION at
// the scan layer (ScanResult::unparsed carries only bad_locus, ANTS-3653
// INV-24/40), so a fixture built that way would leave unparsed[] empty either
// way and assert nothing about the second array.
TEST(DocCitationsExamples, Inv49RegionSuppressesBothArraysAndCounts) {
    expect_reset();
    const auto r = scan(lines({
        "src/a.cpp:1",                            // 1  outside, survives
        kBegin,                                   // 2
        "src/foo.cpp:12 and src/a.cpp:0",         // 3  citation + bad_locus
        kEnd,                                     // 4
        "src/a.cpp:2",                            // 5  outside, survives
    }));
    expect(r.citations.size() == 2, "INV-49: two citations survive", render(r));
    expect(r.unparsed.isEmpty(), "INV-49: the bad_locus token is suppressed", render(r));
    if (r.citations.size() == 2) {
        expect(r.citations[0].docLine == 1, "INV-49: first survivor", render(r));
        expect(r.citations[1].docLine == 5, "INV-49: second survivor", render(r));
    }
    expect(r.examplesSuppressed == 2, "INV-52: counts BOTH arrays", render(r));

    // No marker at all, and a closed region holding no tokens: both are 0, and
    // the second is the row that separates "no region" from "empty region".
    const auto plain = scan(lines({"src/a.cpp:1"}));
    expect(plain.examplesSuppressed == 0, "INV-52: marker-free is 0", render(plain));
    const auto empty = scan(lines({"src/a.cpp:1", kBegin, "prose", kEnd}));
    expect(empty.examplesSuppressed == 0, "INV-52: empty region is 0", render(empty));
    expect(empty.citations.size() == 1, "INV-52: empty region suppresses nothing",
           render(empty));
    EXPECT_EQ(0, expect_failures());
}

// INV-50 — the three boundary cases the mask has to get right.
TEST(DocCitationsExamples, Inv50FenceSpanAndDocLineBoundaries) {
    expect_reset();

    // (a) A marker inside a FENCE is not a marker. The fenced sample carries a
    // `begin` and NO `end` on purpose: with a pair, a fence-blind mask would
    // open and close the region inside the fence and the post-fence citation
    // would survive anyway — the wrong implementation would pass, and this is
    // the only fence-mask guard there is.
    const auto a = scan(lines({
        "```markdown",                            // 1
        kBegin,                                   // 2  sample text, not syntax
        "```",                                    // 3
        "src/foo.cpp:12",                         // 4
    }));
    expect(a.citations.size() == 1, "INV-50a: fenced begin opens nothing", render(a));
    expect(a.examplesSuppressed == 0, "INV-50a: nothing suppressed", render(a));
    expect(a.unterminatedExamples == -1, "INV-50a: no open region", render(a));

    // (b) A marker inside a MULTI-LINE inline code span IS a marker — the mask
    // consumes `fence` only, deliberately (§ 2.2).
    const auto b = scan(lines({
        "text `abc",                              // 1  span opens
        kBegin,                                   // 2  inside the span, honoured
        "def` more",                              // 3  span closes
        "src/foo.cpp:12",                         // 4  suppressed by the region
    }));
    expect(b.citations.isEmpty(), "INV-50b: span-borne marker opens a region", render(b));
    expect(b.examplesSuppressed == 1, "INV-50b: the citation was dropped", render(b));
    expect(b.unterminatedExamples == 2, "INV-50b: reports its opener", render(b));

    // (c) A citation is tested by its OWN docLine, not its span's opening line.
    // The span opens on line 1 (unmasked); the citation's text is on line 3
    // (masked). Keyed on the span, this citation would survive.
    const auto c = scan(lines({
        "`foo",                                   // 1  span opens, NOT masked
        kBegin,                                   // 2
        "src/a.cpp:1`",                           // 3  docLine 3, masked
        kEnd,                                     // 4
    }));
    expect(c.citations.isEmpty(), "INV-50c: keyed on docLine, suppressed", render(c));
    expect(c.examplesSuppressed == 1, "INV-50c: counted", render(c));

    // (d) The mirror: span opens INSIDE the region, the citation's text lands
    // after the `end`. Keyed on the span, this citation would be suppressed.
    const auto d = scan(lines({
        kBegin,                                   // 1
        "`foo",                                   // 2  span opens, masked
        kEnd,                                     // 3
        "src/a.cpp:1`",                           // 4  docLine 4, NOT masked
    }));
    expect(d.citations.size() == 1, "INV-50d: keyed on docLine, emitted", render(d));
    expect(d.examplesSuppressed == 0, "INV-50d: nothing dropped", render(d));
    EXPECT_EQ(0, expect_failures());
}

// INV-51 — unterminated and degenerate markers.
TEST(DocCitationsExamples, Inv51UnterminatedAndDegenerateMarkers) {
    expect_reset();

    // Unterminated: masks to end of input, reports the 1-based opener.
    const auto open = scan(lines({
        "a", "b", kBegin, "c", "d", "e", "f", "g", "src/a.cpp:1",   // begin @3, cite @9
    }));
    expect(open.unterminatedExamples == 3, "INV-51: opener line", render(open));
    expect(open.citations.isEmpty(), "INV-51: masks to EOF", render(open));

    // The same line list, closed at 6 — the `end` must precede the citation, or
    // the row would assert suppression rather than closure.
    const auto closed = scan(lines({
        "a", "b", kBegin, "c", "d", kEnd, "f", "g", "h", "src/a.cpp:1",
    }));
    expect(closed.unterminatedExamples == -1, "INV-51: balanced", render(closed));
    expect(closed.citations.size() == 1, "INV-51: citation survives", render(closed));

    // A stray `end` is ignored and masks nothing.
    const auto stray = scan(lines({kEnd, "src/a.cpp:1"}));
    expect(stray.unterminatedExamples == -1, "INV-51: stray end is inert", render(stray));
    expect(stray.citations.size() == 1, "INV-51: stray end masks nothing", render(stray));

    // begin/begin/end — defeats a DEPTH COUNTER, which would leave the region
    // open after the single `end` and swallow the trailing citation.
    const auto doubled = scan(lines({
        kBegin, kBegin, "src/a.cpp:1", kEnd, "src/a.cpp:2",
    }));
    expect(doubled.citations.size() == 1, "INV-51: no depth counter", render(doubled));
    expect(doubled.unterminatedExamples == -1, "INV-51: closed by one end", render(doubled));

    // Unclosed doubled `begin` — the ONLY row that separates "ignored" from
    // "re-opens": a re-opening implementation is byte-identical to the correct
    // one on the row above, and reports 5 here instead of 2.
    const auto reopen = scan(lines({"a", kBegin, "b", "c", kBegin, "d"}));
    expect(reopen.unterminatedExamples == 2, "INV-51: keeps the FIRST opener",
           render(reopen));

    // An unclosed FENCE swallows a later `end`: the region stays open, and both
    // sentinels fire with the fence as the cause (§ 2.2).
    const auto swallowed = scan(lines({
        "a", kBegin, "```", "x", "y", kEnd, "z", "w", "src/a.cpp:1",
    }));
    expect(swallowed.unterminatedFence == 3, "INV-51: fence reported", render(swallowed));
    expect(swallowed.unterminatedExamples == 2, "INV-51: region still open",
           render(swallowed));
    expect(swallowed.citations.isEmpty(), "INV-51: nothing survives", render(swallowed));
    EXPECT_EQ(0, expect_failures());
}

// INV-54 — the three mechanical spelling rules, from BOTH sides. The accept row
// is what fixes the indent boundary: without it, an implementation matching
// `^<!--` with no indent allowance passes every reject row.
TEST(DocCitationsExamples, Inv54MarkerSpellingBothSides) {
    expect_reset();

    struct Row {
        const char *label;
        const char *marker;
    };
    const Row rejects[] = {
        {"uppercase",      "<!-- DOC-EXAMPLES: BEGIN -->"},
        {"four spaces",    "    <!-- doc-examples: begin -->"},
        {"tab indent",     "\t<!-- doc-examples: begin -->"},
        {"trailing prose", "<!-- doc-examples: begin --> and more"},
    };
    for (const Row &row : rejects) {
        const auto r = scan(lines({row.marker, "src/a.cpp:1"}));
        if (r.citations.size() != 1 || r.examplesSuppressed != 0)
            ADD_FAILURE() << "INV-54 reject row '" << row.label
                          << "' opened a region: " << render(r).toStdString();
    }

    // Accept side: three spaces is still a marker.
    const auto ok = scan(lines({
        "   <!-- doc-examples: begin -->", "src/a.cpp:1", "   <!-- doc-examples: end -->",
    }));
    expect(ok.citations.isEmpty(), "INV-54: 3-space indent IS a marker", render(ok));
    expect(ok.examplesSuppressed == 1, "INV-54: and it suppressed", render(ok));
    EXPECT_EQ(0, expect_failures());
}

// INV-53 — a masked line resets the sticky antecedent, exactly as a fenced line
// does; a stray `end` masks nothing and so resets nothing. Check-layer: the
// antecedent lives in the read path. `src/a.cpp` must EXIST, or a no-reset
// implementation would return missing_file rather than ok and the fixture would
// distinguish nothing. The continuation is `:1` so it is `ok` rather than
// out_of_range whatever the fixture file's length.
TEST(DocCitationsExamples, Inv53MaskedLineResetsAntecedent) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "int a = 1;\n");

    const QString reset = fx.doc(lines({
        "src/a.cpp:1", kBegin, kEnd, "`:1`",
    }).toUtf8());
    const QJsonObject r = DocCitations::check(fx.root, reset);
    expect(doccit_test::cites(r).size() == 2, "INV-53: two citations",
           doccit_test::render(r));
    if (doccit_test::cites(r).size() == 2) {
        expect(doccit_test::status(r, 0) == QLatin1String("ok"),
               "INV-53: antecedent is real", doccit_test::render(r));
        expect(doccit_test::status(r, 1) == QLatin1String("unresolved"),
               "INV-53: the region reset it", doccit_test::render(r));
    }

    // A stray `end` masks nothing, so the antecedent survives it.
    const QString survives = fx.write(QStringLiteral("stray.md"), lines({
        "src/a.cpp:1", kEnd, "`:1`",
    }).toUtf8());
    const QJsonObject s = DocCitations::check(fx.root, survives);
    expect(doccit_test::cites(s).size() == 2, "INV-53: two citations",
           doccit_test::render(s));
    if (doccit_test::cites(s).size() == 2)
        expect(doccit_test::status(s, 1) == QLatin1String("ok"),
               "INV-53: stray end does not reset", doccit_test::render(s));
    EXPECT_EQ(0, expect_failures());
}

// INV-55 — the JSON keys omit at their sentinels and appear otherwise, and
// suppressed tokens are absent from every tally. The in-region `6.2:1` is the
// row that makes `unparsed_total:0` falsifiable: it would be an `unresolvable`
// unparsed[] entry in the check response if it were not suppressed.
TEST(DocCitationsExamples, Inv55JsonKeysAndTallies) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "int a = 1;\n");

    const auto has = [](const QJsonObject &o, const char *k) {
        return o.contains(QString::fromLatin1(k));
    };

    // Marker-free: neither key present.
    const QJsonObject plain =
        DocCitations::check(fx.root, fx.doc(QByteArray("src/a.cpp:1\n")));
    expect(!has(plain, "examples_suppressed"), "INV-55: omitted at 0",
           doccit_test::render(plain));
    expect(!has(plain, "unterminated_examples"), "INV-55: omitted at -1",
           doccit_test::render(plain));

    // One suppressed citation, one suppressed would-be-unparsed token, one
    // survivor: every tally counts only the survivor.
    const QString mixed = fx.write(QStringLiteral("mixed.md"), lines({
        "src/a.cpp:1", kBegin, "src/gone.cpp:1 and `6.2:1`", kEnd,
    }).toUtf8());
    const QJsonObject m = DocCitations::check(fx.root, mixed);
    expect(m.value(QStringLiteral("examples_suppressed")).toInt() == 2,
           "INV-55: both dropped tokens counted", doccit_test::render(m));
    expect(m.value(QStringLiteral("count")).toInt() == 1, "INV-55: count is survivors",
           doccit_test::render(m));
    expect(m.value(QStringLiteral("counts")).toObject()
               .value(QStringLiteral("ok")).toInt() == 1,
           "INV-55: counts bucket is survivors", doccit_test::render(m));
    expect(m.value(QStringLiteral("unparsed_total")).toInt() == 0,
           "INV-55: the 6.2:1 never reached unparsed[]", doccit_test::render(m));

    // Unterminated region: the key appears, carrying the opener.
    const QString runaway = fx.write(QStringLiteral("runaway.md"), lines({
        "a", "b", kBegin, "src/a.cpp:1",
    }).toUtf8());
    const QJsonObject u = DocCitations::check(fx.root, runaway);
    expect(u.value(QStringLiteral("unterminated_examples")).toInt() == 3,
           "INV-55: opener echoed to JSON", doccit_test::render(u));

    // Truncation: `unterminated_examples` is relative to the SCANNED PREFIX, so
    // a region closed past the cut still reports. scanned_lines < doc_lines is
    // what tells a reader the document may in fact be balanced.
    QStringList thirty;
    for (int i = 1; i <= 30; ++i)
        thirty << (i == 5 ? QString::fromUtf8(kBegin)
                          : i == 25 ? QString::fromUtf8(kEnd)
                                    : QStringLiteral("line %1").arg(i));
    const QString cut = fx.write(QStringLiteral("cut.md"),
                                 thirty.join(QLatin1Char('\n')).toUtf8());
    Options opts;
    opts.maxDocLines = 10;                       // below the handler floor: engine call
    const QJsonObject t = DocCitations::check(fx.root, cut, opts);
    expect(t.value(QStringLiteral("unterminated_examples")).toInt() == 5,
           "INV-55: opener within the prefix", doccit_test::render(t));
    expect(t.value(QStringLiteral("scanned_lines")).toInt() == 10
               && t.value(QStringLiteral("doc_lines")).toInt() == 30,
           "INV-55: truncation is visible", doccit_test::render(t));
    EXPECT_EQ(0, expect_failures());
}
