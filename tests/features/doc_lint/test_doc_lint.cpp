// ANTS-3663 — doc_lint ENGINE conformance test. Phase-1 rows only: INV-1,
// INV-3, INV-4, INV-7, INV-9, INV-10, INV-17, INV-19, INV-20. The fix-path rows
// (INV-5/6/12/14/15/16/18/21) belong to ANTS-3669 and its own directory.
//
// ANTS-5506 — the sixth checker, doc_facts, adds five engine rows here:
// INV-22 (count_mismatch), INV-23 (invariant_duplicate), INV-24
// (leaked_markup), INV-25 (version_drift) and INV-27 (verb_arg_unknown). Its
// two verb-layer rows (INV-26, INV-28) live in doc_lint_verb/ because
// cmdDocLint needs a live MainWindow. THE CODE DOES NOT EXIST YET at the time
// these rows were written: DocLint::Options carries no projectVersion or
// verbArgs member, and "doc_facts" is not in DocLint::checkNames(). Every row
// below is expected to fail to COMPILE until that lands — see the ANTS-3663
// bug description for the exact interface these rows are written against.
//
// Filesystem-shaped by nature: two of the five checkers are frozen engines that
// re-read the document themselves, so a temp tree is the only honest fixture.
// The root comes from `../doc_citations/fixture.h` rather than a local copy —
// that header's own comment says why, and a divergent copy would make these
// rows pass or fail for reasons that have nothing to do with doc_lint.
//
// Several rows assert an ABSENCE and so pass trivially against an engine that
// produces nothing, which is exactly the state they first run in. spec.md's
// table records what each mutation actually turned red.

#include "doclint.h"

#include "../doc_citations/fixture.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

namespace {

using doccit_test::Fixture;

const QString kIntegrity = QStringLiteral("doc_integrity");
const QString kCitations = QStringLiteral("doc_citations");
const QString kDedup     = QStringLiteral("doc_dedup");
const QString kSymbols   = QStringLiteral("doc_symbols");
const QString kSpecLint  = QStringLiteral("spec_lint");
const QString kDocFacts  = QStringLiteral("doc_facts");

DocLint::Options baseOpts(const Fixture &fx) {
    DocLint::Options o;
    o.rootCanonical        = fx.root;
    o.symbols.rootCanonical = fx.root;
    o.specsDirRel          = QStringLiteral("docs/specs");
    return o;
}

int countKind(const DocLint::Result &r, const QString &verb, const QString &kind) {
    int n = 0;
    for (const DocFinding::Finding &f : r.findings)
        if (f.verb == verb && f.kind == kind) ++n;
    return n;
}

int countVerb(const DocLint::Result &r, const QString &verb) {
    int n = 0;
    for (const DocFinding::Finding &f : r.findings)
        if (f.verb == verb) ++n;
    return n;
}

bool skippedWith(const DocLint::Result &r, const QString &file, const QString &reason) {
    for (const DocLint::Skip &s : r.skipped)
        if (s.file == file && s.reason == reason) return true;
    return false;
}

// Failure detail: a red line should say what came back, not only which
// predicate failed.
std::string render(const DocLint::Result &r) {
    QStringList out;
    for (const DocFinding::Finding &f : r.findings)
        out << QStringLiteral("%1 %2 %3:%4 [%5] %6")
                   .arg(f.verb, f.kind, f.file, QString::number(f.line),
                        QString::number(f.emissionIndex), f.message);
    for (const DocLint::Skip &s : r.skipped)
        out << QStringLiteral("SKIP %1 %2").arg(s.file, s.reason);
    for (const DocLint::CheckError &e : r.checkErrors)
        out << QStringLiteral("ERR %1 %2 %3").arg(e.verb, e.file, e.reason);
    return out.join(QStringLiteral(" | ")).toStdString();
}

}  // namespace

// INV-1 — one shared read serves all three NATIVE checkers. Every document sits
// under specs_dir so spec_lint is eligible for each: without that the
// counterfactual is six rather than nine, and the number this row states stops
// discriminating. An implementation that reads once per checker returns nine.
TEST(DocLint, Inv1SharedReadForNativeCheckers) {
    Fixture fx;
    const QStringList docs{QStringLiteral("docs/specs/a.md"),
                           QStringLiteral("docs/specs/b.md"),
                           QStringLiteral("docs/specs/c.md")};
    fx.write(docs.at(0), "# A\n\nalpha prose about nothing much at all\n");
    fx.write(docs.at(1), "# B\n\nbeta prose about nothing much at all\n");
    fx.write(docs.at(2), "# C\n\ngamma prose about nothing much at all\n");

    DocLint::Probe probe;
    DocLint::Options o = baseOpts(fx);
    o.checks = {kDedup, kSymbols, kSpecLint};
    o.probe  = &probe;

    const DocLint::Result r = DocLint::run(docs, o);

    EXPECT_EQ(3, probe.opens) << "one open per document, serving three checkers";
    EXPECT_EQ(docs, r.checkedDocs);
}

// INV-3 — an ok citation is never a finding, INCLUDING one whose anchor moved.
// The middle citation is the row that fails against an adapter treating the
// advisory anchor flag as a status.
TEST(DocLint, Inv3OkCitationIsNeverAFinding) {
    Fixture fx;
    fx.write(QStringLiteral("src/real.cpp"), "int alpha() { return 1; }\n");
    const QString doc = QStringLiteral("docs/d.md");
    fx.write(doc,
             "# D\n"
             "\n"
             "healthy `src/real.cpp:1`\n"
             "anchored `Widget::paintEvent` in `src/real.cpp:1`\n"
             "gone `src/gone.cpp:1`\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kCitations};
    const DocLint::Result r = DocLint::run({doc}, o);

    EXPECT_EQ(1, countVerb(r, kCitations)) << render(r);
    EXPECT_EQ(1, countKind(r, kCitations, QStringLiteral("missing_file"))) << render(r);
}

// INV-4 — unparsed[] entries never become findings, and are not lost either.
// Both halves: without the first the verb re-imports the drowning problem
// ANTS-3659 was written to fix, and without the second an adapter that discards
// unparsed[] entirely passes.
TEST(DocLint, Inv4UnparsedIsNeverAFinding) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/u.md");
    fx.write(doc, "# U\n\nsee section `6.2:1` for the rest\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kCitations};
    const DocLint::Result r = DocLint::run({doc}, o);

    EXPECT_EQ(0, countVerb(r, kCitations)) << render(r);
    EXPECT_EQ(1, r.stats.unparsedTotal) << render(r);
}

// INV-7 — a TOTAL order. The tied pair is asserted at the ENGINE, not on the
// wire: two findings agreeing on all five serialised fields are
// indistinguishable in the response, so swapping them changes no byte and the
// two-runs arm alone proves nothing. A five-key comparator passes that arm and
// fails this one.
TEST(DocLint, Inv7TotalDocumentOrder) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/o.md");
    // Identical link text AND identical target, so the two findings agree on
    // file, line, verb, kind and message — every serialised key.
    fx.write(doc,
             "# O\n"
             "\n"
             "[same](nope.md) and [same](nope.md)\n"
             "\n"
             "a citation `src/absent.cpp:2` too\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kIntegrity, kCitations};

    const DocLint::Result r1 = DocLint::run({doc}, o);
    const DocLint::Result r2 = DocLint::run({doc}, o);

    ASSERT_EQ(2, countKind(r1, kIntegrity, QStringLiteral("broken_link"))) << render(r1);

    // Two runs agree, key for key.
    ASSERT_EQ(r1.findings.size(), r2.findings.size());
    for (int i = 0; i < r1.findings.size(); ++i) {
        EXPECT_EQ(r1.findings.at(i).file,    r2.findings.at(i).file);
        EXPECT_EQ(r1.findings.at(i).line,    r2.findings.at(i).line);
        EXPECT_EQ(r1.findings.at(i).verb,    r2.findings.at(i).verb);
        EXPECT_EQ(r1.findings.at(i).kind,    r2.findings.at(i).kind);
        EXPECT_EQ(r1.findings.at(i).message, r2.findings.at(i).message);
    }

    // The discriminating half: the tied pair is in ascending emissionIndex.
    int prev = -1, seen = 0;
    for (const DocFinding::Finding &f : r1.findings) {
        if (f.verb != kIntegrity || f.kind != QStringLiteral("broken_link")) continue;
        EXPECT_LT(prev, f.emissionIndex) << render(r1);
        prev = f.emissionIndex;
        ++seen;
    }
    EXPECT_EQ(2, seen);

    // The five keys are ordered as stated, ahead of the tiebreak.
    for (int i = 1; i < r1.findings.size(); ++i) {
        const DocFinding::Finding &a = r1.findings.at(i - 1);
        const DocFinding::Finding &b = r1.findings.at(i);
        const bool ordered =
            std::tie(a.file, a.line, a.verb, a.kind, a.message, a.emissionIndex) <=
            std::tie(b.file, b.line, b.verb, b.kind, b.message, b.emissionIndex);
        EXPECT_TRUE(ordered) << render(r1);
    }
}

// INV-9 — doc_citations' whole-document alarms survive the adapter. This is the
// row that fails against the obvious implementation, an adapter that walks
// citations[] and nothing else; /cold-eyes reads the fence alarm from this
// field, so losing it breaks the caller this verb was built for.
TEST(DocLint, Inv9WholeDocumentAlarmsSurvive) {
    Fixture fx;
    const QString fence = QStringLiteral("docs/fence.md");
    const QString examples = QStringLiteral("docs/examples.md");
    const QString closed = QStringLiteral("docs/closed.md");

    fx.write(fence, "# F\n\n```cpp\nint x = 0;\n");
    fx.write(examples, "# E\n\n<!-- doc-examples: begin -->\nsome sample text\n");
    fx.write(closed,
             "# C\n"
             "\n"
             "<!-- doc-examples: begin -->\n"
             "see `src/one.cpp:1` here\n"
             "see `src/two.cpp:1` here\n"
             "<!-- doc-examples: end -->\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kCitations};
    const DocLint::Result r = DocLint::run({closed, examples, fence}, o);

    ASSERT_EQ(1, countKind(r, kCitations, QStringLiteral("unterminated_fence"))) << render(r);
    ASSERT_EQ(1, countKind(r, kCitations, QStringLiteral("unterminated_examples"))) << render(r);
    for (const DocFinding::Finding &f : r.findings) {
        if (f.kind == QStringLiteral("unterminated_fence")) {
            EXPECT_EQ(fence, f.file);
            EXPECT_EQ(3, f.line) << "the opener's line";
        }
        if (f.kind == QStringLiteral("unterminated_examples")) {
            EXPECT_EQ(examples, f.file);
            EXPECT_EQ(3, f.line) << "the opener's line";
        }
    }

    // A statistic, never a finding.
    EXPECT_EQ(2, r.stats.examplesSuppressed) << render(r);
    EXPECT_EQ(0, countKind(r, kCitations, QStringLiteral("examples_suppressed"))) << render(r);
}

// INV-9's second half — an incompleteness degrades the answer rather than
// describing it, so it routes to check_errors[] and NOT to check_stats. The
// citation findings that were produced stay valid.
TEST(DocLint, Inv9IncompletenessRoutesToCheckErrors) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/long.md");
    QByteArray body = "# L\n\ncite `src/absent.cpp:1` here\n";
    for (int i = 0; i < 40; ++i) body += "filler line\n";
    fx.write(doc, body);

    DocLint::Options o = baseOpts(fx);
    o.checks = {kCitations};
    o.citations.maxDocLines = 4;   // below the document's own length

    const DocLint::Result r = DocLint::run({doc}, o);

    int truncatedErrors = 0;
    for (const DocLint::CheckError &e : r.checkErrors)
        if (e.reason == QStringLiteral("citations_truncated") && e.file == doc)
            ++truncatedErrors;
    EXPECT_EQ(1, truncatedErrors) << render(r);
    // Still ran, so it is still in checks_run — the caller must read the two
    // together, which is what the pairing asserts.
    EXPECT_TRUE(r.checksRun.contains(kCitations)) << render(r);
}

// INV-10 — a checker's trouble on one document does not stop the walk, and its
// incompleteness is visible. Five assertions because containment is
// two-dimensional: per document AND per checker, and an implementation that
// gets one axis right routinely gets the other wrong.
TEST(DocLint, Inv10CheckerFailureIsContained) {
    Fixture fx;
    const QString bad  = QStringLiteral("docs/bad.md");
    const QString ok1  = QStringLiteral("docs/ok1.md");
    const QString ok2  = QStringLiteral("docs/ok2.md");

    QByteArray longDoc = "# Bad\n\ncite `src/absent.cpp:1` here\n[dead](nowhere.md)\n";
    for (int i = 0; i < 40; ++i) longDoc += "filler line\n";
    fx.write(bad, longDoc);
    fx.write(ok1, "# One\n\ncite `src/gone1.cpp:1` here\n");
    fx.write(ok2, "# Two\n\ncite `src/gone2.cpp:1` here\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kCitations, kIntegrity};
    o.citations.maxDocLines = 4;   // bites on `bad` alone

    const DocLint::Result r = DocLint::run({bad, ok1, ok2}, o);

    // 1. exactly one entry, naming that {verb, file}
    ASSERT_EQ(1, r.checkErrors.size()) << render(r);
    EXPECT_EQ(kCitations, r.checkErrors.at(0).verb);
    EXPECT_EQ(bad,        r.checkErrors.at(0).file);
    // 2. the verb is still in checks_run, because it did run
    EXPECT_TRUE(r.checksRun.contains(kCitations)) << render(r);
    // 3. findings still present from it on the OTHER documents
    int otherDocCitationFindings = 0;
    for (const DocFinding::Finding &f : r.findings)
        if (f.verb == kCitations && f.file != bad) ++otherDocCitationFindings;
    EXPECT_EQ(2, otherDocCitationFindings) << render(r);
    // 4. findings still present from the OTHER checker on the troubled document
    int otherCheckerOnBad = 0;
    for (const DocFinding::Finding &f : r.findings)
        if (f.verb == kIntegrity && f.file == bad) ++otherCheckerOnBad;
    EXPECT_GE(otherCheckerOnBad, 1) << render(r);
    // 5. the walk completed
    EXPECT_EQ(3, r.checkedDocs.size()) << render(r);
}

// INV-17 — each checker runs only on the documents it is eligible for, and an
// ineligible document is not a failure. The second call is the discriminating
// one: an implementation that filters by silently emitting nothing leaves
// spec_lint in checks_run and reports a clean spec set that was never checked.
TEST(DocLint, Inv17EligibilityFiltersWithoutSkipping) {
    Fixture fx;
    const QString spec  = QStringLiteral("docs/specs/ANTS-1.md");
    const QString std1  = QStringLiteral("docs/standards/one.md");
    const QString read1 = QStringLiteral("docs/readme.md");

    // An invariant with no test surface — a spec_lint finding by construction.
    fx.write(spec, "# ANTS-1\n\n## 3. Invariants\n\n- **INV-1** — a claim with no test clause\n");
    fx.write(std1, "# One\n\nprose that is not a spec at all\n");
    fx.write(read1, "# Readme\n\nmore prose that is not a spec\n");

    DocLint::Options o = baseOpts(fx);
    const DocLint::Result r = DocLint::run({spec, std1, read1}, o);

    // spec_lint findings come from the spec only.
    for (const DocFinding::Finding &f : r.findings)
        if (f.verb == kSpecLint) { EXPECT_EQ(spec, f.file) << render(r); }
    // Neither non-spec is a skip or an error — nothing failed.
    EXPECT_FALSE(skippedWith(r, std1, QStringLiteral("read_failed"))) << render(r);
    EXPECT_TRUE(r.checkedDocs.contains(std1)) << render(r);
    EXPECT_TRUE(r.checkedDocs.contains(read1)) << render(r);
    for (const DocLint::CheckError &e : r.checkErrors)
        EXPECT_NE(kSpecLint, e.verb) << render(r);

    // check_stats aggregation: line_count is MERGED per checked spec, keyed by
    // project-relative path. A composer treating it as a scalar overwrites it
    // once per document and returns the last one, which looks entirely
    // plausible in an envelope.
    EXPECT_EQ(1, r.stats.lineCount.size()) << render(r);
    EXPECT_TRUE(r.stats.lineCount.contains(spec)) << render(r);

    // The discriminating call: no spec in scope at all.
    const DocLint::Result none = DocLint::run({std1, read1}, o);
    EXPECT_FALSE(none.checksRun.contains(kSpecLint)) << render(none);
    EXPECT_TRUE(none.checksRun.contains(kIntegrity)) << render(none);
    EXPECT_TRUE(none.checksRun.contains(kCitations)) << render(none);
    EXPECT_TRUE(none.checksRun.contains(kDedup)) << render(none);
    EXPECT_TRUE(none.checksRun.contains(kSymbols)) << render(none);
}

// INV-19 — every document the run does not check is named, and the response
// says it is incomplete. The truncated half is the one an implementation gets
// wrong: it is easy to list a skipped file and still report a response that
// reads as a complete account of the tree.
TEST(DocLint, Inv19UncheckedDocumentsAreNamed) {
    Fixture fx;
    const QString good = QStringLiteral("docs/a.md");
    const QString unreadable = QStringLiteral("docs/b.md");
    const QString capped1 = QStringLiteral("docs/c.md");
    const QString capped2 = QStringLiteral("docs/d.md");

    fx.write(good, "# A\n\n[dead](nowhere.md)\n");
    // A DIRECTORY where a document is expected: QFile::open fails without a
    // chmod, so the row behaves the same for a root and a non-root runner.
    QDir().mkpath(fx.root + QLatin1Char('/') + unreadable);
    fx.write(capped1, "# C\n\nprose\n");
    fx.write(capped2, "# D\n\nprose\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kIntegrity};
    o.walk.maxDocsPerRun = 2;

    const DocLint::Result r =
        DocLint::run({good, unreadable, capped1, capped2}, o);

    EXPECT_TRUE(skippedWith(r, unreadable, QStringLiteral("read_failed"))) << render(r);
    EXPECT_TRUE(skippedWith(r, capped1, QStringLiteral("doc_cap"))) << render(r);
    EXPECT_TRUE(skippedWith(r, capped2, QStringLiteral("doc_cap"))) << render(r);
    EXPECT_FALSE(r.checkedDocs.contains(unreadable)) << render(r);
    EXPECT_FALSE(r.checkedDocs.contains(capped1)) << render(r);
    EXPECT_TRUE(r.truncated) << "a cap elided a document";
    EXPECT_GE(countKind(r, kIntegrity, QStringLiteral("broken_link")), 1)
        << "findings still present from the checked document";
}

// INV-20 — a citation finding is filed against the document that CONTAINS it,
// never the target it names. The last two assertions are what fail against the
// coin-flip adapter: filing under the target yields a findings list whose file
// values are not documents and were never enumerated.
TEST(DocLint, Inv20CitationFilesUnderItsDocument) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/a.md");
    const QString target = QStringLiteral("src/gone.cpp");
    fx.write(doc, "# A\n\nline two\n\nsee `src/gone.cpp:400` for the rest\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kCitations};
    const DocLint::Result r = DocLint::run({doc}, o);

    ASSERT_EQ(1, countVerb(r, kCitations)) << render(r);
    const DocFinding::Finding &f = r.findings.at(0);
    EXPECT_EQ(doc, f.file);
    EXPECT_EQ(5, f.line);
    EXPECT_TRUE(f.message.contains(target))
        << "the target is named in message, the only place it appears";
    EXPECT_TRUE(r.checkedDocs.contains(doc));
    EXPECT_FALSE(r.checkedDocs.contains(target));
}

// ANTS-5506 INV-22 — count_mismatch fires only when both sides resolve.
// (a) "The three steps:" + two bullets -> mismatch (claims three, lists two).
// (b) "The three steps:" + three bullets, one carrying a nested bullet ->
//     matches (nested items are not counted, so the top-level count is three).
// (c) "Two of the three steps:" + two bullets -> not a claim (two cardinals).
// (d) "The three steps:" + a paragraph -> a claim, but not followed by a list.
// (e) (a)'s text inside a fence -> not a claim (fenced).
TEST(DocLint, Inv22CountMismatchNeedsBothSides) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/counts.md");
    fx.write(doc,
             "# Doc\n"
             "\n"
             "The three steps:\n"          // line 3 — (a), mismatch
             "- one\n"
             "- two\n"
             "\n"
             "The three steps:\n"          // line 7 — (b), matches
             "- one\n"
             "- two\n"
             "  - nested\n"
             "- three\n"
             "\n"
             "Two of the three steps:\n"   // line 13 — (c), two cardinals
             "- one\n"
             "- two\n"
             "\n"
             "The three steps:\n"          // line 17 — (d), no list follows
             "Paragraph text, not a list.\n"
             "\n"
             "```text\n"                   // line 20 — (e), fenced
             "The three steps:\n"
             "- one\n"
             "- two\n"
             "```\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kDocFacts};
    const DocLint::Result r = DocLint::run({doc}, o);

    ASSERT_EQ(1, countKind(r, kDocFacts, QStringLiteral("count_mismatch"))) << render(r);
    const DocFinding::Finding &f = r.findings.at(0);
    EXPECT_EQ(doc, f.file);
    EXPECT_EQ(3, f.line) << "filed at the lead-in line, (a)";
    EXPECT_TRUE(f.message.contains(QStringLiteral("3")) || f.message.contains(QStringLiteral("three")))
        << "message must name the claimed number: " << render(r);
    EXPECT_TRUE(f.message.contains(QStringLiteral("2")) || f.message.contains(QStringLiteral("two")))
        << "message must name the counted number: " << render(r);

    // (a), (b) and (d) are claims; (c) has two cardinals and (e) is fenced.
    EXPECT_EQ(3, r.stats.countClaimsChecked) << render(r);
    EXPECT_EQ(QStringList{kDocFacts}, r.checksRun);
}

// ANTS-5506 INV-22, the claim and the list as measured on this repo's docs.
// Each case below was a false count_mismatch before the rule it names; the
// last is the positive guard, without which every absence here is vacuous.
TEST(DocLint, Inv22CountClaimIsOneSentence) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/claims.md");
    fx.write(doc,
             "# Doc\n\n"
             // A numbered item's own marker is not a count.
             "2. Scan these markers:\n   - a\n   - b\n   - c\n\n"
             // A number after a capitalised word is a label.
             "Phase 7 covers:\n\n- a\n- b\n\n"
             // A number joined to another token is not standalone.
             "The loop-2 edits:\n\n- a\n- b\n- c\n\n"
             // `one` makes it a sum, not a claim of three.
             "Three problems and one other:\n\n- a\n- b\n- c\n- d\n\n"
             // The tail of a wrapped paragraph: its sentence began earlier.
             "This paragraph wraps across lines and its tail\n"
             "mentions six widgets as an aside:\n\n- a\n- b\n\n"
             // A lead-in that is itself an item: the parent's next item ends it.
             "- Three signal hookups:\n  - a\n  - b\n  - c\n- next parent item\n\n"
             // A change of marker kind starts a new list.
             "Two region scopes:\n\n- a\n- b\n\n1. x\n2. y\n\n"
             // Positive guard: only the last sentence is the claim, and it is wrong.
             "Some context here. Two new files:\n\n- only one\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kDocFacts};
    const DocLint::Result r = DocLint::run({doc}, o);

    ASSERT_EQ(1, countKind(r, kDocFacts, QStringLiteral("count_mismatch"))) << render(r);
    const QStringList lines = QStringLiteral(
        "# Doc\n\n2. Scan these markers:\n   - a\n   - b\n   - c\n\n"
        "Phase 7 covers:\n\n- a\n- b\n\nThe loop-2 edits:\n\n- a\n- b\n- c\n\n"
        "Three problems and one other:\n\n- a\n- b\n- c\n- d\n\n"
        "This paragraph wraps across lines and its tail\n"
        "mentions six widgets as an aside:\n\n- a\n- b\n\n"
        "- Three signal hookups:\n  - a\n  - b\n  - c\n- next parent item\n\n"
        "Two region scopes:\n\n- a\n- b\n\n1. x\n2. y\n\n"
        "Some context here. Two new files:\n").split(QLatin1Char('\n'));
    EXPECT_EQ(lines.indexOf(QStringLiteral("Some context here. Two new files:")) + 1,
              r.findings.at(0).line) << render(r);
}

// ANTS-5506 INV-23 — invariant_duplicate names both definitions. INV-1 is
// defined at line 3 and again at line 9 (the second is the finding); the
// table row, the prose mention and the bullet under "## 7. Notes" are all
// excluded — a table row and prose never match the bullet anchor, and a
// bullet under a non-Invariants heading never fires.
TEST(DocLint, Inv23DuplicateInvariantNamesBoth) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/dupinv.md");
    fx.write(doc,
             "## 3. Invariants\n"                                    // 1
             "\n"                                                    // 2
             "- **INV-1** \xe2\x80\x94 first definition\n"           // 3
             "\n"                                                    // 4
             "| INV-1 | something |\n"                               // 5
             "\n"                                                    // 6
             "Prose mentioning INV-1.\n"                             // 7
             "\n"                                                    // 8
             "- **INV-1** \xe2\x80\x94 second definition (duplicate)\n"  // 9
             "\n"                                                    // 10
             "## 7. Notes\n"                                         // 11
             "\n"                                                    // 12
             "- **INV-1** \xe2\x80\x94 a bullet under a different heading\n");  // 13

    DocLint::Options o = baseOpts(fx);
    o.checks = {kDocFacts};
    const DocLint::Result r = DocLint::run({doc}, o);

    ASSERT_EQ(1, countKind(r, kDocFacts, QStringLiteral("invariant_duplicate"))) << render(r);
    const DocFinding::Finding &f = r.findings.at(0);
    EXPECT_EQ(doc, f.file);
    EXPECT_EQ(9, f.line) << "filed at the SECOND definition";
    EXPECT_TRUE(f.message.contains(QStringLiteral("3")))
        << "message must name the first definition's line: " << render(r);
}

// ANTS-5506 INV-24 — leaked_markup ignores code. Six lines: a bare opening
// <invoke>; the same tag inside backticks (excluded); a fence opener; the
// same tag inside the fence (excluded); the fence closer; a bare closing
// </function_results>. Two findings: lines one and six.
TEST(DocLint, Inv24LeakedMarkupIgnoresCode) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/leaked.md");
    fx.write(doc,
             "<invoke>\n"                 // 1 — bare, finding
             "`<invoke>`\n"               // 2 — inline code span, excluded
             "```\n"                      // 3 — fence opener
             "<invoke>\n"                 // 4 — inside fence, excluded
             "```\n"                      // 5 — fence closer
             "</function_results>\n");    // 6 — bare, finding

    DocLint::Options o = baseOpts(fx);
    o.checks = {kDocFacts};
    const DocLint::Result r = DocLint::run({doc}, o);

    ASSERT_EQ(2, countKind(r, kDocFacts, QStringLiteral("leaked_markup"))) << render(r);
    QList<int> lines;
    for (const DocFinding::Finding &f : r.findings)
        if (f.kind == QStringLiteral("leaked_markup")) lines << f.line;
    std::sort(lines.begin(), lines.end());
    ASSERT_EQ(2, lines.size());
    EXPECT_EQ(1, lines.at(0));
    EXPECT_EQ(6, lines.at(1));
}

// ANTS-5506 INV-25 — version_drift reads only claims, against the injected
// version. Three plain-text lines (a mismatched "Version <strong>", a
// matching "current version" phrase, and a non-claim "since"); the first
// repeated inside backticks; a CHANGELOG.md that is never checked; and an
// empty projectVersion that disables the kind entirely.
TEST(DocLint, Inv25VersionDriftReadsOnlyClaims) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/v.md");
    fx.write(doc,
             "# V\n"
             "\n"
             "Version <strong>1.2.2</strong> here.\n"       // line 3 — mismatch
             "\n"
             "the current version is 1.2.3, noted.\n"       // line 5 — matches
             "\n"
             "since 1.0.0 this held.\n"                     // line 7 — not a claim
             "\n"
             "`Version <strong>1.2.2</strong> here.`\n");   // line 9 — inline code, excluded
    const QString changelog = QStringLiteral("CHANGELOG.md");
    fx.write(changelog, "# Changelog\n\nthe current version is 0.1.0 today.\n");

    DocLint::Options o = baseOpts(fx);
    o.checks = {kDocFacts};
    o.projectVersion = QStringLiteral("1.2.3");
    const DocLint::Result r = DocLint::run({doc, changelog}, o);

    ASSERT_EQ(1, countKind(r, kDocFacts, QStringLiteral("version_drift"))) << render(r);
    const DocFinding::Finding &f = r.findings.at(0);
    EXPECT_EQ(doc, f.file);
    EXPECT_EQ(3, f.line) << "filed at the first (mismatched) claim";
    for (const DocFinding::Finding &g : r.findings)
        EXPECT_NE(changelog, g.file) << "CHANGELOG.md is never checked: " << render(r);

    DocLint::Options empty = o;
    empty.projectVersion.clear();
    const DocLint::Result r2 = DocLint::run({doc, changelog}, empty);
    EXPECT_EQ(0, countKind(r2, kDocFacts, QStringLiteral("version_drift"))) << render(r2);
    EXPECT_TRUE(r2.stats.versionUnavailable) << render(r2);
}

// ANTS-5506 INV-27 — verb_arg_unknown checks top-level keys against the
// injected map. verbArgs is {roadmap_query: [id, section, caller_cwd,
// fields]}. (a) a {…} object with a typo'd key -> finding. (b) key:value
// tokens, both known -> none. (c) a nested key inside a {…} value -> not
// checked (top-level only). (d) a verb absent from the map -> not checked.
// (e) the verb followed by anything else -> not a call shape, not checked.
TEST(DocLint, Inv27VerbArgsTopLevelOnly) {
    Fixture fx;
    const QString doc = QStringLiteral("docs/calls.md");
    fx.write(doc,
             "# Calls\n"
             "\n"
             "(a) `roadmap_query {id:\"X\", sectoin:\"a\"}`\n"   // line 3 — finding
             "\n"
             "(b) `roadmap_query id:\"X\" fields:[\"a\"]`\n"     // line 5 — clean
             "\n"
             "(c) `roadmap_query {section:{bogus:1}}`\n"         // line 7 — nested, not checked
             "\n"
             "(d) `other_verb {x:1}`\n"                          // line 9 — unknown verb
             "\n"
             "(e) `roadmap_query sectoin`\n");                   // line 11 — not a call shape

    DocLint::Options o = baseOpts(fx);
    o.checks = {kDocFacts};
    o.verbArgs = {{QStringLiteral("roadmap_query"),
                   {QStringLiteral("id"), QStringLiteral("section"),
                    QStringLiteral("caller_cwd"), QStringLiteral("fields")}}};
    const DocLint::Result r = DocLint::run({doc}, o);

    ASSERT_EQ(1, countKind(r, kDocFacts, QStringLiteral("verb_arg_unknown"))) << render(r);
    const DocFinding::Finding &f = r.findings.at(0);
    EXPECT_EQ(doc, f.file);
    EXPECT_EQ(3, f.line) << "filed at (a)";
    EXPECT_TRUE(f.message.contains(QStringLiteral("sectoin")))
        << "message must name the unknown key: " << render(r);
    EXPECT_EQ(3, r.stats.verbCallsChecked) << "(a), (b) and (c) — " << render(r);

    DocLint::Options noSchema = baseOpts(fx);
    noSchema.checks = {kDocFacts};
    const DocLint::Result r2 = DocLint::run({doc}, noSchema);
    EXPECT_EQ(0, countKind(r2, kDocFacts, QStringLiteral("verb_arg_unknown"))) << render(r2);
    EXPECT_TRUE(r2.stats.schemaUnavailable) << render(r2);
}

// Not a contract — a re-runnable MEASUREMENT, so a later sweep can ask "does
// this still produce these figures?" rather than "do these passages agree?".
// Disabled by default because it walks the real corpus. qInfo is suppressed in
// the test bundles, hence fprintf.
//
// Run with:
//   ./test_core --gtest_also_run_disabled_tests --gtest_filter='DocLint.DISABLED_CorpusCalibration'
TEST(DocLint, DISABLED_CorpusCalibration) {
    const QString root = QString::fromUtf8(ANTS_PROJECT_ROOT_PATH);
    const QDir rootDir(root);
    QStringList docs;
    QDirIterator it(rootDir.filePath(QStringLiteral("docs")),
                    {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) docs << rootDir.relativeFilePath(it.next());
    docs.sort();

    DocLint::Probe probe;
    DocLint::Options o;
    o.rootCanonical         = root;
    o.symbols.rootCanonical = root;
    o.specsDirRel           = QStringLiteral("docs/specs");
    o.probe                 = &probe;

    QElapsedTimer timer;
    timer.start();
    const DocLint::Result r = DocLint::run(docs, o);
    const qint64 ms = timer.elapsed();

    fprintf(stderr, "\n=== doc_lint corpus calibration ===\n");
    fprintf(stderr, "enumerated       %d\n", int(docs.size()));
    fprintf(stderr, "checked          %d\n", int(r.checkedDocs.size()));
    fprintf(stderr, "skipped          %d\n", int(r.skipped.size()));
    fprintf(stderr, "check_errors     %d\n", int(r.checkErrors.size()));
    fprintf(stderr, "findings         %d\n", int(r.findings.size()));
    fprintf(stderr, "elapsed_ms       %lld\n", static_cast<long long>(ms));
    // The SHARED read only. The two adapted engines open their own files and
    // expose no counter, so six of the spec's nine-open budget are unobservable
    // from here — which is why § 2.1 states it as a budget and asserts three.
    fprintf(stderr, "shared_opens     %d  (adapters' opens are not instrumented)\n",
            probe.opens);

    for (const QString &verb : DocLint::checkNames()) {
        int n = 0;
        for (const DocFinding::Finding &f : r.findings) if (f.verb == verb) ++n;
        fprintf(stderr, "  %-16s %5d  %s\n", qPrintable(verb), n,
                r.checksRun.contains(verb) ? "ran" : "DID NOT RUN");
    }
    fprintf(stderr, "passages_total   %d\n", r.stats.passagesTotal);
    fprintf(stderr, "passages_compared %d\n", r.stats.passagesCompared);
    fprintf(stderr, "symbols total/resolved/unresolved/not_checked  %d/%d/%d/%d\n",
            r.stats.symbolsTotal, r.stats.symbolsResolved,
            r.stats.symbolsUnresolved, r.stats.symbolsNotChecked);
    fprintf(stderr, "unparsed_total   %d\n", r.stats.unparsedTotal);
    fprintf(stderr, "sections_checked %s\n", r.stats.sectionsChecked ? "true" : "false");
    fprintf(stderr, "specs line_count entries %d\n", int(r.stats.lineCount.size()));
    SUCCEED();
}
