// Feature-conformance test for spec.md — ANTS-4445.
//
// parseActionableFindings is a pure function over report text, so every
// invariant here is behavioural. The dialog wiring that calls it is covered
// by INV-6's shape assertion plus the dialog's own test.

#include "testauditengine.h"

#include "../../_support/expect.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>

ANTS_TEST_SCOPE();

namespace {

QJsonObject only(const QJsonArray &a) {
    return a.isEmpty() ? QJsonObject() : a.at(0).toObject();
}

}  // namespace

TEST(TestAuditActionableParse, Inv1FieldsFoldInReads) {
    expect_reset();

    const QStringList reports = {
        QStringLiteral("## Assertions (3)\n"
                       "- [HIGH] tests/features/foo/test_foo.cpp:42 — the "
                       "assertion cannot fail\n")
    };
    const QJsonArray out = TestAuditEngine::parseActionableFindings(reports);
    ASSERT_EQ(1, out.size()) << "INV-1: exactly one finding expected";

    const QJsonObject f = only(out);
    expect(f.value(QStringLiteral("severity")).toString()
               == QLatin1String("high"),
           "INV-1: severity");
    expect(f.value(QStringLiteral("file")).toString()
               == QLatin1String("tests/features/foo/test_foo.cpp"),
           "INV-1: file");
    expect(f.value(QStringLiteral("line")).toInt() == 42, "INV-1: line");
    expect(f.value(QStringLiteral("summary")).toString()
               == QLatin1String("the assertion cannot fail"),
           "INV-1: summary");
    expect(f.value(QStringLiteral("dimension")).toString()
               == QLatin1String("assertions"),
           "INV-1: dimension");

    // foldIn refuses the WHOLE batch on one missing headline, so a finding
    // with no description must never be emitted.
    const QJsonArray noSummary = TestAuditEngine::parseActionableFindings(
        { QStringLiteral("## Naming\n- [LOW] a/b.cpp:1 — \n") });
    expect(noSummary.isEmpty(),
           "INV-1: a finding with no summary is dropped, not emitted");

    EXPECT_EQ(0, expect_failures());
}

TEST(TestAuditActionableParse, Inv2DimensionFromHeader) {
    expect_reset();

    // Emoji and a trailing count are decoration, not part of the value.
    // QString::fromUtf8, not QStringLiteral: the latter does NOT decode
    // \x UTF-8 escapes, so the parser would be handed mangled bytes and the
    // test would be measuring something other than what it claims.
    const QJsonArray decorated = TestAuditEngine::parseActionableFindings(
        { QString::fromUtf8("## \xF0\x9F\xA7\xAA Naming (7)\n"
                            "- [MED] a/b.cpp:3 \xE2\x80\x94 name says nothing\n") });
    expect(only(decorated).value(QStringLiteral("dimension")).toString()
               == QLatin1String("naming"),
           "INV-2: emoji and trailing count are stripped, case normalised");

    // An unrecognised header keeps its own text rather than vanishing.
    const QJsonArray unknown = TestAuditEngine::parseActionableFindings(
        { QStringLiteral("## Wibble\n- [LOW] a/b.cpp:9 — something\n") });
    expect(only(unknown).value(QStringLiteral("dimension")).toString()
               == QLatin1String("Wibble"),
           "INV-2: an unknown dimension header is preserved verbatim");

    // A finding before any header still parses; it simply has no dimension.
    const QJsonArray headerless = TestAuditEngine::parseActionableFindings(
        { QStringLiteral("- [LOW] a/b.cpp:9 — orphan\n") });
    ASSERT_EQ(1, headerless.size())
        << "INV-2: a finding with no preceding header is still a finding";
    expect(only(headerless).value(QStringLiteral("dimension"))
               .toString().isEmpty(),
           "INV-2: and carries an empty dimension rather than a wrong one");

    EXPECT_EQ(0, expect_failures());
}

TEST(TestAuditActionableParse, Inv3AllDashFormsAccepted) {
    expect_reset();

    // The prompt asks for an em dash; a model asked for one routinely writes
    // a hyphen or an en dash. Dropping those would rebuild the very silence
    // this feature removes.
    // QString::fromUtf8, not QStringLiteral — see INV-2. Caught here by the
    // em and en forms parsing to zero findings against a correct parser.
    for (const QString &dash : { QString::fromUtf8("\xE2\x80\x94"),   // em
                                 QString::fromUtf8("\xE2\x80\x93"),   // en
                                 QString::fromUtf8("-") }) {
        const QJsonArray out = TestAuditEngine::parseActionableFindings(
            { QStringLiteral("## Flakiness\n- [HIGH] a/b.cpp:5 %1 sleeps\n")
                  .arg(dash) });
        expect(out.size() == 1
                   && only(out).value(QStringLiteral("summary")).toString()
                          == QLatin1String("sleeps"),
               "INV-3: this dash form parses",
               QStringLiteral("dash form %1 produced %2 finding(s)")
                   .arg(dash).arg(out.size()));
    }

    EXPECT_EQ(0, expect_failures());
}

TEST(TestAuditActionableParse, Inv4SeverityCanonicalised) {
    expect_reset();

    struct Case { const char *in; const char *out; };
    for (const Case &c : { Case{"CRITICAL", "crit"}, Case{"CRIT", "crit"},
                           Case{"MEDIUM", "med"},   Case{"MED", "med"},
                           Case{"HIGH", "high"},    Case{"LOW", "low"},
                           Case{"INFO", "info"} }) {
        const QJsonArray out = TestAuditEngine::parseActionableFindings(
            { QStringLiteral("## Coverage_gaps\n- [%1] a/b.cpp:1 — x\n")
                  .arg(QLatin1String(c.in)) });
        expect(out.size() == 1
                   && only(out).value(QStringLiteral("severity")).toString()
                          == QLatin1String(c.out),
               "INV-4: severity canonicalised",
               QStringLiteral("[%1] should canonicalise to %2")
                   .arg(QLatin1String(c.in), QLatin1String(c.out)));
    }

    EXPECT_EQ(0, expect_failures());
}

TEST(TestAuditActionableParse, Inv5ColonInPathSurvives) {
    expect_reset();

    const QJsonArray out = TestAuditEngine::parseActionableFindings(
        { QStringLiteral("## Isolation\n"
                         "- [LOW] odd:name/test.cpp:17 — leaks an env var\n") });
    ASSERT_EQ(1, out.size()) << "INV-5: the finding must still parse";
    expect(only(out).value(QStringLiteral("file")).toString()
               == QLatin1String("odd:name/test.cpp"),
           "INV-5: only the trailing :<digits> is the line number");
    expect(only(out).value(QStringLiteral("line")).toInt() == 17,
           "INV-5: and that line is read correctly");

    EXPECT_EQ(0, expect_failures());
}

TEST(TestAuditActionableParse, Inv6NoFindingsIsEmptyNotGarbage) {
    expect_reset();

    // Prose, a heading, and a bullet that is not a finding. None of it may
    // become a finding: foldIn would allocate a ROADMAP id for each.
    const QJsonArray out = TestAuditEngine::parseActionableFindings(
        { QStringLiteral("# Chunk report\n\n"
                         "## Summary\n\n"
                         "The tests in this chunk look reasonable.\n"
                         "- no issues found\n"
                         "- [NOTE] not a severity\n") });
    expect(out.isEmpty(),
           "INV-6: nothing that is not a finding becomes one");

    expect(TestAuditEngine::parseActionableFindings({}).isEmpty(),
           "INV-6: no reports yields no findings");

    EXPECT_EQ(0, expect_failures());
}
