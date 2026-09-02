// ANTS-3665 — `test_surface` must come out of BOTH invariant forms.
//
// `SpecParse::parseSpecBody` is pure (spec text in, JSON out), so every fixture
// here is a string literal — no temp dir, no filesystem. That this file links
// at all is INV-5: the parser now lives in ants_core_lib rather than in
// remotecontrol.cpp's anonymous namespace.
//
// See spec.md for the contract; docs/standards/specs.md § 6 owns the promise
// this test holds the parser to.

#include "specparse.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>

#include "../../_support/expect.h"
ANTS_TEST_SCOPE();

namespace {

QJsonObject parse(const char *utf8) {
    return SpecParse::parseSpecBody(QString::fromUtf8(utf8));
}

QJsonObject inv(const QJsonObject &parsed, int idx) {
    return parsed.value(QStringLiteral("invariants")).toArray().at(idx).toObject();
}

QString field(const QJsonObject &o, const char *key) {
    return o.value(QLatin1String(key)).toString();
}

bool has(const QJsonObject &o, const char *key) {
    return o.contains(QLatin1String(key));
}

// Every fixture needs the header the parser anchors on.
QString withHeader(const QString &invariantsSection) {
    return QStringLiteral("# ANTS-9999 — a fixture spec\n"
                          "\n"
                          "**Status:** draft.\n"
                          "**Kind:** implement.\n"
                          "\n"
                          "## 3. Invariants\n"
                          "\n")
           + invariantsSection;
}

QString render(const QJsonObject &o) {
    return QStringLiteral("body=<%1> test_surface=<%2> present=%3")
        .arg(field(o, "body"), field(o, "test_surface"),
             has(o, "test_surface") ? QStringLiteral("yes") : QStringLiteral("no"));
}

}  // namespace

// INV-1 — a bullet-form invariant carrying a `*Test:*` clause yields
// `test_surface`, and the clause is removed from `body`. This is the whole
// defect: before ANTS-3665 only the GFM table branch set the field, so the
// overwhelming majority of this corpus — which uses the bullet form the same
// standard calls the default — returned the clause buried inside `body`.
TEST(SpecParseTestSurface, Inv1BulletFormEmitsTestSurface) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "- **INV-1** — The widget refuses a negative count. "
                       "*Test:* pass -1 → `bad_args`.\n"))
            .toUtf8()
            .constData());

    expect(p.value(QStringLiteral("invariants_count")).toInt() == 1,
           "INV-1/one-invariant-parsed",
           QString::number(p.value(QStringLiteral("invariants_count")).toInt()));

    const auto i0 = inv(p, 0);
    expect(field(i0, "id") == QStringLiteral("INV-1"), "INV-1/id", render(i0));
    expect(has(i0, "test_surface"), "INV-1/test_surface-present", render(i0));
    expect(field(i0, "test_surface")
               == QStringLiteral("pass -1 → `bad_args`."),
           "INV-1/test_surface-value", render(i0));
    // The clause must leave `body`, matching the table form where the two are
    // disjoint cells. A parser that merely *copies* the clause into the new
    // field would pass the assertion above and fail this one.
    expect(field(i0, "body")
               == QStringLiteral("The widget refuses a negative count."),
           "INV-1/body-excludes-clause", render(i0));
    ASSERT_EQ(0, expect_finish());
}

// INV-2 — an invariant with no test clause omits the key entirely. Absence is
// the signal: ANTS-3662's `invariant_no_test` check asks exactly this question,
// and an empty string would make "has none" indistinguishable from "has one
// that is blank".
TEST(SpecParseTestSurface, Inv2NoClauseOmitsTheKey) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "- **INV-1** — The widget refuses a negative count.\n"))
            .toUtf8()
            .constData());

    const auto i0 = inv(p, 0);
    expect(!has(i0, "test_surface"), "INV-2/key-absent", render(i0));
    expect(field(i0, "body")
               == QStringLiteral("The widget refuses a negative count."),
           "INV-2/body-intact", render(i0));
    ASSERT_EQ(0, expect_finish());
}

// INV-3 — the GFM table form is unchanged. The fix is additive; a regression
// here would mean the bullet work broke the branch that already worked.
TEST(SpecParseTestSurface, Inv3TableFormStillWorks) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "| INV | Claim | Test surface |\n"
                       "|---|---|---|\n"
                       "| INV-1 | Refuses a negative count | pass -1 → bad_args |\n"))
            .toUtf8()
            .constData());

    const auto i0 = inv(p, 0);
    expect(field(i0, "id") == QStringLiteral("INV-1"), "INV-3/id", render(i0));
    expect(field(i0, "test_surface") == QStringLiteral("pass -1 → bad_args"),
           "INV-3/test_surface", render(i0));
    expect(field(i0, "body") == QStringLiteral("Refuses a negative count"),
           "INV-3/body", render(i0));
    ASSERT_EQ(0, expect_finish());
}

// INV-4 — the clause ends at its paragraph, not at the end of the invariant.
// Bullets in this corpus routinely carry further paragraphs arguing *why* the
// invariant is shaped the way it is; that prose is about the invariant, not
// about how to test it, so it stays in `body`. A naive "everything after
// `*Test:*`" implementation swallows it into the test surface, which is why
// this fixture has a trailing paragraph and INV-1's does not.
TEST(SpecParseTestSurface, Inv4ClauseEndsAtItsParagraph) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "- **INV-1** — The widget refuses a negative count. "
                       "*Test:* pass -1 → `bad_args`.\n"
                       "\n"
                       "  The refusal is deliberate: a negative count reads as "
                       "\"unbounded\" to the caller.\n"))
            .toUtf8()
            .constData());

    const auto i0 = inv(p, 0);
    expect(field(i0, "test_surface")
               == QStringLiteral("pass -1 → `bad_args`."),
           "INV-4/clause-stops-at-paragraph", render(i0));
    expect(field(i0, "body").contains(QStringLiteral("deliberate")),
           "INV-4/commentary-kept-in-body", render(i0));
    expect(!field(i0, "test_surface").contains(QStringLiteral("deliberate")),
           "INV-4/commentary-not-in-test-surface", render(i0));
    expect(field(i0, "body")
               .startsWith(QStringLiteral("The widget refuses a negative count.")),
           "INV-4/claim-still-leads-body", render(i0));
    ASSERT_EQ(0, expect_finish());
}

// INV-5 — several bullets in one section each get their own clause. Guards the
// splice against an off-by-one in the bullet-boundary scan, which is the part
// of this parser most likely to mis-slice once bodies stop being one line.
TEST(SpecParseTestSurface, Inv5MultipleBulletsEachKeepTheirOwnClause) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "- **INV-1** — First claim. *Test:* first probe.\n"
                       "- **INV-2** — Second claim. *Test:* second probe.\n"
                       "- **INV-3** — Third claim, untested.\n"))
            .toUtf8()
            .constData());

    expect(p.value(QStringLiteral("invariants_count")).toInt() == 3,
           "INV-5/three-parsed",
           QString::number(p.value(QStringLiteral("invariants_count")).toInt()));

    const auto i0 = inv(p, 0), i1 = inv(p, 1), i2 = inv(p, 2);
    expect(field(i0, "test_surface") == QStringLiteral("first probe."),
           "INV-5/first-clause", render(i0));
    expect(field(i1, "test_surface") == QStringLiteral("second probe."),
           "INV-5/second-clause", render(i1));
    expect(!has(i2, "test_surface"), "INV-5/third-has-none", render(i2));
    expect(field(i1, "body") == QStringLiteral("Second claim."),
           "INV-5/second-body-clean", render(i1));
    ASSERT_EQ(0, expect_finish());
}

// ANTS-3697 — a bullet-form body terminates at the next ATX heading as well as
// at the next `- **INV-` bullet. Without the clamp the LAST invariant before a
// subheading swallowed the heading and the prose under it; the pattern that
// triggers it — grouping withdrawn invariants under their own `###` — is what
// the permanent-id rule in specs.md pushes authors toward, so it is common
// rather than exotic. Only the one invariant immediately preceding the heading
// is affected, which is why nothing else flagged it.
TEST(SpecParseTestSurface, Ants3697BodyStopsAtNextHeading) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "- **INV-1** — The first one. *Test:* t1.\n"
                       "- **INV-2** — The last live one. *Test:* t2.\n"
                       "\n"
                       "### Withdrawn invariants\n"
                       "\n"
                       "INV-3 was withdrawn in review; its id is retained so\n"
                       "later references keep resolving.\n"))
            .toUtf8().constData());

    const QJsonObject last = inv(p, 1);
    expect(field(last, "id") == QStringLiteral("INV-2"),
           "ANTS-3697: INV-2 parses as its own entry");
    expect(!field(last, "body").contains(QStringLiteral("Withdrawn invariants")),
           "ANTS-3697: the body must not swallow the following heading");
    expect(!field(last, "body").contains(QStringLiteral("retained so")),
           "ANTS-3697: the body must not swallow the prose under the heading");
    expect(field(last, "test_surface") == QStringLiteral("t2."),
           "ANTS-3697: test_surface must not inherit the over-run");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3697 guard — the clamp must not truncate a legitimate multi-paragraph
// body. A heading can never be a continuation of a bullet, but a blank line
// followed by more prose can be, and those bodies are the norm in this corpus.
TEST(SpecParseTestSurface, Ants3697MultiParagraphBodySurvives) {
    expect_reset();
    const auto p = parse(
        withHeader(QStringLiteral(
                       "- **INV-1** — The rule. *Test:* t1.\n"
                       "\n"
                       "  Second paragraph explaining why the rule is shaped\n"
                       "  the way it is.\n"))
            .toUtf8().constData());

    expect(field(inv(p, 0), "body").contains(QStringLiteral("Second paragraph")),
           "ANTS-3697: a multi-paragraph body must survive the heading clamp");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3676 — the clause marker was located by its FIRST occurrence in the
// bullet body, with no inline-code masking. A spec that DISCUSSES the marker
// quotes it in backticks, so its body was cut at the mention: everything
// before became `body` and everything after became `test_surface`.
//
// Reproduced live against a spec whose invariant defines `invariant_no_test`
// and therefore has to name the marker. The sharpest consequence is that
// spec_lint's own `invariant_no_test` check consumes this parser, so it read a
// corrupt surface on exactly the specs most likely to discuss spec format.
TEST(SpecParseTestSurface, Ants3676QuotedMarkerIsNotTheClause) {
    expect_reset();
    const QJsonObject p = parse(withHeader(QStringLiteral(
        "- **INV-2** — Every `INV-N` without a `*Test:*` clause is reported.\n"
        "\n"
        "  *Test:* a fixture spec with a clauseless invariant yields one\n"
        "  finding.\n")).toUtf8().constData());
    const QJsonObject i = inv(p, 0);
    expect(field(i, "id") == QStringLiteral("INV-2"),
           "ANTS-3676 precondition: the invariant parsed", render(i));
    expect(field(i, "body").contains(QStringLiteral("is reported")),
           "ANTS-3676: the body survives past the quoted marker", render(i));
    expect(field(i, "test_surface").startsWith(QStringLiteral("a fixture spec")),
           "ANTS-3676: the REAL clause is the test surface", render(i));
    expect(!field(i, "test_surface").contains(QStringLiteral("clause is reported")),
           "ANTS-3676: the prose after the mention is not the test surface",
           render(i));
    EXPECT_EQ(0, expect_finish());
}

// The converse, and the one that keeps the fix honest: an invariant that only
// MENTIONS the marker has no test surface, and must still report none. That
// absence is the signal spec_lint's invariant_no_test check reads, so masking
// must not manufacture a surface from the mention.
TEST(SpecParseTestSurface, Ants3676QuotedMarkerAloneIsNoSurface) {
    expect_reset();
    const QJsonObject p = parse(withHeader(QStringLiteral(
        "- **INV-3** — A bullet naming `*Test:*` in prose declares nothing.\n"))
        .toUtf8().constData());
    const QJsonObject i = inv(p, 0);
    expect(!has(i, "test_surface"),
           "ANTS-3676: a quoted mention alone yields no test_surface",
           render(i));
    expect(field(i, "body").contains(QStringLiteral("declares nothing")),
           "ANTS-3676: and the body is not truncated at the mention",
           render(i));
    EXPECT_EQ(0, expect_finish());
}
