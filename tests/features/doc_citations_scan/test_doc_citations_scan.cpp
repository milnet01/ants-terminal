// ANTS-3653 — citation-scan conformance test. The scan is a pure function
// (document text in, tokens out), so every fixture here is a string literal and
// an expected token list: no temp dir, no seeded index, no filesystem at all.
// See spec.md for the invariant map and docs/specs/ANTS-3653.md for the
// grammar.

#include "doccitations.h"

#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

#include "../../_support/expect.h"
ANTS_TEST_SCOPE();

using DocCitations::Citation;
using DocCitations::Options;
using DocCitations::ScanResult;
using DocCitations::Unparsed;

namespace {

QStringList doc(const char *utf8) {
    return QString::fromUtf8(utf8).split(QLatin1Char('\n'));
}

ScanResult scan(const char *utf8, const Options &opts = {}) {
    return DocCitations::scan(doc(utf8), opts);
}

// Compact "what did it find" rendering, for failure details.
QString render(const ScanResult &r) {
    QString s;
    for (const Citation &c : r.citations)
        s += QStringLiteral("[%1:%2 %3 %4-%5%6%7%8] ")
                 .arg(c.docLine).arg(c.docCol)
                 .arg(c.path.isEmpty() ? QStringLiteral("<cont>") : c.path)
                 .arg(c.startLine).arg(c.endLine)
                 .arg(c.approximate ? QStringLiteral(" ~") : QString())
                 .arg(c.partial ? QStringLiteral(" +") : QString())
                 .arg(c.continuation ? QStringLiteral(" cont") : QString());
    for (const Unparsed &u : r.unparsed)
        s += QStringLiteral("{%1 %2 %3} ").arg(u.docLine).arg(u.raw).arg(u.reason);
    return s.isEmpty() ? QStringLiteral("<nothing>") : s;
}

}  // namespace

// INV-1 — fenced blocks are skipped; INDENTED ones are not (fenceMask models
// no indented code blocks, so a citation in one is harvested as a live claim).
// All three tokens are written directory-bearing so the same fixture text is
// reusable by ANTS-3636's resolution tests.
TEST(DocCitationsScan, Inv1FencedSkippedIndentedHarvested) {
    expect_reset();
    const auto r = scan(
        "# H\n"
        "```cpp\n"
        "`src/a.cpp:1`\n"
        "```\n"
        "prose src/a.cpp:2 here\n"
        "\n"
        "    src/a.cpp:6 indented\n");
    expect(r.citations.size() == 2, "INV-1/count", render(r));
    if (r.citations.size() == 2) {
        expect(r.citations[0].startLine == 2, "INV-1/prose-locus", render(r));
        expect(r.citations[0].docLine == 5, "INV-1/prose-line", render(r));
        expect(r.citations[1].startLine == 6, "INV-1/indented-locus", render(r));
        expect(r.citations[1].docLine == 7, "INV-1/indented-line", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-2 — the inline-span policy INVERTS vs doc_integrity: a span is exactly
// where the citation data is. Bold, link text and bare prose all harvest too.
TEST(DocCitationsScan, Inv2HarvestedFromSpanBoldLinkTextAndProse) {
    expect_reset();
    const auto r = scan(
        "a `src/a.cpp:2` b\n"
        "c **`src/a.cpp:3`** d\n"
        "e [`src/a.cpp:4`](x) f\n"
        "g src/a.cpp:5 h\n");
    expect(r.citations.size() == 4, "INV-2/count", render(r));
    for (int i = 0; i < r.citations.size(); ++i) {
        expect(r.citations[i].path == QStringLiteral("src/a.cpp"),
               "INV-2/path", render(r));
        expect(r.citations[i].startLine == i + 2, "INV-2/locus", render(r));
        expect(!r.citations[i].continuation, "INV-2/not-continuation", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-3 — the corpus writes both separators; a hyphen-only rule loses a large
// minority of the ranges. Both carry the path: written bare they would be
// continuations and the assertion would be about the sticky rule instead.
TEST(DocCitationsScan, Inv3HyphenAndEnDashRanges) {
    expect_reset();
    const auto r = scan(
        "hyphen src/a.cpp:10-12 here\n"
        "en dash src/a.cpp:10\xE2\x80\x93""12 here\n");
    expect(r.citations.size() == 2, "INV-3/count", render(r));
    if (r.citations.size() == 2) {
        expect(r.citations[0].startLine == 10 && r.citations[0].endLine == 12,
               "INV-3/hyphen", render(r));
        expect(r.citations[1].startLine == 10 && r.citations[1].endLine == 12,
               "INV-3/en-dash", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-9 — trailing loci are dropped, not guessed: the first locus is kept and
// `partial` records that something was dropped.
TEST(DocCitationsScan, Inv9TrailingLociDropped) {
    expect_reset();
    const auto r = scan(
        "a src/a.cpp:262/265/286 b\n"
        "c src/a.cpp:5669+ d\n");
    expect(r.citations.size() == 2, "INV-9/count", render(r));
    if (r.citations.size() == 2) {
        expect(r.citations[0].startLine == 262 && r.citations[0].endLine == 262,
               "INV-9/slash-locus", render(r));
        expect(r.citations[0].partial, "INV-9/slash-partial", render(r));
        expect(r.citations[1].startLine == 5669, "INV-9/plus-locus", render(r));
        expect(r.citations[1].partial, "INV-9/plus-partial", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-10 — `~` is an echo so a reader can see the author hedged. It is set from
// either KEPT locus and changes nothing else; on a DROPPED trailing locus it is
// dropped with it.
TEST(DocCitationsScan, Inv10TildeOnKeptLocusOnly) {
    expect_reset();
    const auto plain = scan("see src/a.cpp:752 here\n");
    const auto tilde = scan("see src/a.cpp:~752 here\n");
    expect(plain.citations.size() == 1 && tilde.citations.size() == 1,
           "INV-10/counts", render(tilde));
    if (plain.citations.size() == 1 && tilde.citations.size() == 1) {
        const Citation &p = plain.citations[0];
        const Citation &t = tilde.citations[0];
        // Identical apart from `approximate` and `raw`.
        expect(!p.approximate && t.approximate, "INV-10/flag", render(tilde));
        expect(p.raw == QStringLiteral("src/a.cpp:752")
                   && t.raw == QStringLiteral("src/a.cpp:~752"),
               "INV-10/raw", t.raw);
        expect(p.path == t.path && p.startLine == t.startLine
                   && p.endLine == t.endLine && p.partial == t.partial
                   && p.docLine == t.docLine && p.docCol == t.docCol
                   && p.continuation == t.continuation,
               "INV-10/otherwise-identical", render(tilde));
    }

    const auto onEnd = scan("see src/a.cpp:208-~228 here\n");
    expect(onEnd.citations.size() == 1 && onEnd.citations[0].approximate
               && onEnd.citations[0].startLine == 208
               && onEnd.citations[0].endLine == 228,
           "INV-10/range-end-tilde", render(onEnd));

    const auto dropped = scan("see src/a.cpp:262/~265 here\n");
    expect(dropped.citations.size() == 1 && dropped.citations[0].partial
               && !dropped.citations[0].approximate,
           "INV-10/dropped-tilde", render(dropped));
    ASSERT_EQ(0, expect_finish());
}

// INV-24 — stage 1 recognises a malformed locus so stage 2 can REPORT it; and
// the known-unsupported `~:N` spelling (tilde before the colon) matches no
// production at all, so it is invisible to both arrays. Asserted because
// "invisible" is otherwise indistinguishable from "not yet implemented".
TEST(DocCitationsScan, Inv24MalformedLociReportedTildeColonInvisible) {
    expect_reset();
    const auto r = scan(
        "a src/a.cpp:0 b\n"
        "c src/a.cpp:10-5 d\n"
        "e `~:11985` f\n");
    expect(r.citations.isEmpty(), "INV-24/no-citations", render(r));
    expect(r.unparsed.size() == 2, "INV-24/unparsed-count", render(r));
    for (const Unparsed &u : r.unparsed)
        expect(u.reason == QStringLiteral("bad_locus"), "INV-24/reason", u.reason);
    if (r.unparsed.size() == 2) {
        expect(r.unparsed[0].raw == QStringLiteral("src/a.cpp:0"),
               "INV-24/raw-zero", r.unparsed[0].raw);
        expect(r.unparsed[1].raw == QStringLiteral("src/a.cpp:10-5"),
               "INV-24/raw-inverted", r.unparsed[1].raw);
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-29 — `count` is occurrences, not distinct targets.
TEST(DocCitationsScan, Inv29RepeatedCitationIsTwoEntries) {
    expect_reset();
    const auto r = scan("see src/a.cpp:1 and again src/a.cpp:1 here\n");
    expect(r.citations.size() == 2, "INV-29/count", render(r));
    if (r.citations.size() == 2) {
        expect(r.citations[0].docLine == 1 && r.citations[1].docLine == 1,
               "INV-29/same-line", render(r));
        expect(r.citations[0].docCol < r.citations[1].docCol,
               "INV-29/distinct-columns", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-32 — a bare `:N` is a continuation ONLY when it fills a whole inline code
// span. Without this rule the raw-line scan harvests every clock time and ratio
// in the corpus, inherits an unrelated path, and emits the real text of that
// file's line 45 under status "ok".
TEST(DocCitationsScan, Inv32ContinuationMustFillAWholeSpan) {
    expect_reset();
    const auto noCont = scan(
        "see `src/a.cpp:1`\n"
        "at `09:45` the ratio 3:1 held\n");
    expect(noCont.citations.size() == 1, "INV-32/no-cont-count", render(noCont));
    for (const Citation &c : noCont.citations)
        expect(!c.continuation, "INV-32/none-marked", render(noCont));

    // The one-space strip is applied before the "fills the span" test, so
    // `` ` :2 ` `` counts — that is what a renderer shows the author.
    const auto cont = scan(
        "see `src/a.cpp:1`\n"
        "and ` :2 ` too\n");
    expect(cont.citations.size() == 2, "INV-32/cont-count", render(cont));
    if (cont.citations.size() == 2) {
        expect(!cont.citations[0].continuation, "INV-32/first-not-cont",
               render(cont));
        expect(cont.citations[1].continuation, "INV-32/second-is-cont",
               render(cont));
        expect(cont.citations[1].path.isEmpty(), "INV-32/cont-has-no-path",
               render(cont));
        expect(cont.citations[1].raw == QStringLiteral(":2"),
               "INV-32/cont-raw-sans-delimiters", cont.citations[1].raw);
        expect(cont.citations[1].startLine == 2, "INV-32/cont-locus",
               render(cont));
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-33 — document order: ascending docLine, then ascending column. Plus the
// case that fixes doc_line's definition: a citation inside a span that opens on
// one line and closes on the next takes the line ITS OWN text begins on.
TEST(DocCitationsScan, Inv33DocumentOrderAndSpanCrossingLine) {
    expect_reset();
    const char *fixture =
        "first src/z.cpp:9 alone\n"
        "then src/b.cpp:2 and src/a.cpp:3 together\n";
    const auto r = scan(fixture);
    expect(r.citations.size() == 3, "INV-33/count", render(r));
    if (r.citations.size() == 3) {
        expect(r.citations[0].docLine == 1, "INV-33/first-line", render(r));
        expect(r.citations[1].docLine == 2 && r.citations[2].docLine == 2,
               "INV-33/second-line", render(r));
        expect(r.citations[1].docCol < r.citations[2].docCol,
               "INV-33/column-order", render(r));
        expect(r.citations[1].path == QStringLiteral("src/b.cpp"),
               "INV-33/column-order-is-not-alpha", render(r));
    }
    expect(render(scan(fixture)) == render(r), "INV-33/stable", render(r));

    // The span opens on line 1 and closes on line 2; the citation's own text
    // begins on line 2, so that is its docLine — not the span's opening line.
    const auto crossing = scan(
        "a span `opens here\n"
        "and src/a.cpp:7 sits inside it` — closed\n");
    expect(crossing.citations.size() == 1, "INV-33/crossing-count",
           render(crossing));
    if (crossing.citations.size() == 1)
        expect(crossing.citations[0].docLine == 2, "INV-33/crossing-doc-line",
               render(crossing));
    ASSERT_EQ(0, expect_finish());
}

// INV-36 — an unterminated fence is REPORTED, not absorbed. Inherited silently
// a single stray ``` blanks every citation after it and returns a clean-looking
// all-clear over a document the verb barely read. The fact is not recoverable
// from the mask, which is why fenceMask has the opener overload.
TEST(DocCitationsScan, Inv36UnterminatedFenceReported) {
    expect_reset();
    const char *body =
        "# H\n"
        "prose\n"
        "more prose\n"
        "```cpp\n"        // line 4 — the opener
        "int x;\n"
        "int y;\n"
        "int z;\n"
        "int w;\n"
        "see src/a.cpp:5 here\n";
    const auto open = scan(body);
    expect(open.unterminatedFence == 4, "INV-36/opener-line",
           QString::number(open.unterminatedFence));
    expect(open.citations.isEmpty(), "INV-36/masked-to-end", render(open));

    const auto closed = scan(
        "# H\n"
        "prose\n"
        "more prose\n"
        "```cpp\n"
        "int x;\n"
        "```\n"
        "see src/a.cpp:5 here\n");
    expect(closed.unterminatedFence == -1, "INV-36/closed-absent",
           QString::number(closed.unterminatedFence));
    expect(closed.citations.size() == 1, "INV-36/closed-count", render(closed));

    // The trap case: a doc whose FINAL line is the closer ends in a run of
    // `true` exactly like an unclosed one, so a mask-derived rule passes both
    // cases above and fails only this one.
    const auto closesLast = scan(
        "# H\n"
        "prose\n"
        "more prose\n"
        "```cpp\n"
        "int x;\n"
        "```");
    expect(closesLast.unterminatedFence == -1, "INV-36/closes-on-final-line",
           QString::number(closesLast.unterminatedFence));
    ASSERT_EQ(0, expect_finish());
}

// INV-39 — `trailing` is EXHAUSTIVE: only further /-separated loci and a single
// `+` are absorbed, so the `.` closing a sentence is ordinary punctuation and
// sets no flag. The third row must be bare prose — inside a code span the token
// ends at the closing backtick, the `.` is never adjacent, and the assertion
// would hold against an implementation with no exhaustiveness rule at all.
TEST(DocCitationsScan, Inv39PartialSetByTrailingLociAndNothingElse) {
    expect_reset();
    const auto r = scan(
        "a src/a.cpp:262/265 b\n"
        "c src/a.cpp:5669+ d\n"
        "see src/a.cpp:12.\n");
    expect(r.citations.size() == 3, "INV-39/count", render(r));
    if (r.citations.size() == 3) {
        expect(r.citations[0].partial, "INV-39/slash", render(r));
        expect(r.citations[1].partial, "INV-39/plus", render(r));
        expect(!r.citations[2].partial, "INV-39/sentence-dot-is-punctuation",
               render(r));
        expect(r.citations[2].startLine == 12 && r.citations[2].endLine == 12,
               "INV-39/sentence-locus", render(r));
        expect(r.citations[2].raw == QStringLiteral("src/a.cpp:12"),
               "INV-39/sentence-raw", r.citations[2].raw);
    }
    ASSERT_EQ(0, expect_finish());
}

// INV-40 — an over-long digit run is RECOGNISED and then rejected: never a
// citation at a truncated line number, never silently dropped. Stage 2 tests
// the run's LENGTH before converting, so no over-large value reaches an int.
// The strict single-stage spelling also yields zero citations — it just emits
// nothing at all — so the unparsed entry is the whole assertion.
TEST(DocCitationsScan, Inv40OverLongLocusRecognisedThenRejected) {
    expect_reset();
    const auto r = scan("see src/a.cpp:99999999999999 here\n");
    expect(r.citations.isEmpty(), "INV-40/no-citation", render(r));
    expect(r.unparsed.size() == 1, "INV-40/one-unparsed", render(r));
    if (r.unparsed.size() == 1) {
        expect(r.unparsed[0].reason == QStringLiteral("bad_locus"),
               "INV-40/reason", r.unparsed[0].reason);
        expect(r.unparsed[0].raw == QStringLiteral("src/a.cpp:99999999999999"),
               "INV-40/raw", r.unparsed[0].raw);
    }

    // The boundary from the other side: exactly maxLocusDigits digits is a
    // citation. Driven off the Options field, not a literal, so the two sides
    // cannot drift apart.
    const Options opts;
    const QString atCap = QStringLiteral("1").repeated(opts.maxLocusDigits);
    const auto ok = DocCitations::scan(
        QStringList{QStringLiteral("see src/a.cpp:%1 here").arg(atCap)}, opts);
    expect(ok.citations.size() == 1 && ok.unparsed.isEmpty(),
           "INV-40/at-cap-is-a-citation", render(ok));
    if (ok.citations.size() == 1)
        expect(ok.citations[0].startLine == atCap.toInt(), "INV-40/at-cap-locus",
               render(ok));

    // One digit past the cap, same fixture shape → rejected.
    const auto over = DocCitations::scan(
        QStringList{QStringLiteral("see src/a.cpp:%1 here")
                        .arg(atCap + QLatin1Char('1'))},
        opts);
    expect(over.citations.isEmpty() && over.unparsed.size() == 1,
           "INV-40/one-past-cap-rejected", render(over));
    ASSERT_EQ(0, expect_finish());
}

// ANTS-4743 — a zero must say whether it is CLEAN or SILENT.
//
// The grammar recognises `path:line`, and the authoring standard forbids
// authors writing that form — "line numbers are not grounding: they rot on the
// next edit. Cite the symbol." So the one form this can see is the one banned
// where it is most used, and a conforming corpus scans to zero. Measured on two
// specs of 485 and 650 lines, both dense with citations: count 0, every bucket
// 0, and an EMPTY unparsed reinforcing it — nothing was rejected because
// nothing was recognised as a candidate.
//
// The zero is then laundered: check-doc-facts sorts a clean run into its
// findings-empty bucket and review-contract tells its cold lanes the mechanical
// checks are settled, so an unrun check becomes a fact they may not question.
TEST(DocCitationsScan, Ants4743UnrecognisedCitationShapesAreCounted) {
    expect_reset();

    // The reporter's own forms, backticked as they appear in a spec.
    const auto sym = scan("see `serve.py::build_model()` for the shape\n");
    expect(sym.citations.isEmpty(), "4743/symbol-not-a-citation", render(sym));
    expect(sym.unrecognisedCandidates == 1, "4743/symbol-counted", render(sym));

    const auto path = scan("per `docs/specs/LOTTO-0002-local-web-page.md` 4.6\n");
    expect(path.citations.isEmpty(), "4743/path-not-a-citation", render(path));
    expect(path.unrecognisedCandidates == 1, "4743/path-counted", render(path));

    // The recognised form must NOT be counted as unseen — it is seen. Without
    // this the counter could be "every code span" and every assertion above
    // would still pass.
    const auto real = scan("see `src/a.cpp:45` here\n");
    expect(real.citations.size() == 1, "4743/real-is-a-citation", render(real));
    expect(real.unrecognisedCandidates == 0, "4743/real-not-counted", render(real));

    // Nor may ordinary prose in backticks count. The field decides what a ZERO
    // means, so a loose test would make every backticked word evidence of a
    // missed citation and the number useless.
    const auto prose = scan("set `enabled` to `true` before `run`\n");
    expect(prose.citations.isEmpty(), "4743/prose-no-citation", render(prose));
    expect(prose.unrecognisedCandidates == 0, "4743/prose-not-counted",
           render(prose));

    // A document that genuinely cites nothing stays at zero on BOTH numbers —
    // which is what makes the pair able to tell clean from silent.
    const auto empty = scan("just a sentence with no code spans at all\n");
    expect(empty.citations.isEmpty() && empty.unrecognisedCandidates == 0,
           "4743/silent-and-clean-are-distinguishable", render(empty));
    ASSERT_EQ(0, expect_finish());
}
