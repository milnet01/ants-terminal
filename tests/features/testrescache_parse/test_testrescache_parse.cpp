// ANTS-5064 — feature-conformance test for TestResCache::parseCtestOutput.
// Locks the ctest-log parser's failing-test extraction against real ctest
// output. See spec.md.
//
// Why this exists: the per-test regex requires whitespace between the
// dot-fill and the status marker (`\.+\s+(?:\*\*\*)?`), but real ctest
// prints the marker flush against the dots (`...***Failed`) — the file's
// own documented example in testrescache.cpp shows the same shape. So no
// per-test status line for a failing test ever matches, `failingTests` is
// always empty, and multi-word statuses (`Exception: SegFault`) fail for
// the separate reason that the token capture is a single `\S+` token.

#include <gtest/gtest.h>

#include "testrescache.h"

#include <QString>
#include <QStringList>

namespace TRC = TestResCache;

namespace {

QStringList failureNames(const TRC::ParsedTests &t) {
    QStringList names;
    for (const auto &pf : t.failingTests) names.append(pf.name);
    return names;
}

}  // namespace

// INV-1 — a real ***Failed line (dots run directly into the marker, no
// space) yields the test's name in failingTests. Currently red: the
// per-test regex requires whitespace there, so failingTests is empty.
TEST(TestResCacheParse, Inv1FailedLineNoSpaceBeforeMarker) {
    const QString log = QStringLiteral(R"(Test project /build
    Start 2711: AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes
1/1 Test #2711: AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes ........***Failed    0.07 sec

0% tests passed, 1 tests failed out of 1

Total Test time (real) =   0.07 sec

The following tests FAILED:
	2711 - AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes (Failed) fast features
)");

    const TRC::ParsedTests t = TRC::parseCtestOutput(log);
    ASSERT_TRUE(t.recognised);
    const QStringList names = failureNames(t);
    EXPECT_TRUE(names.contains(QStringLiteral(
        "AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes")))
        << "failingTests did not contain the failed test's name; "
           "failingTests = [" << names.join(", ").toStdString() << "]";
}

// INV-2 — a mixed log (passes, one ***Failed, one ***Exception: SegFault,
// one ***Timeout) names every non-passing test and no passing one.
TEST(TestResCacheParse, Inv2MixedLogNamesEveryNonPassingTest) {
    const QString log = QStringLiteral(R"(Test project /build
1/6 Test #101: SomeSuite.PassOne .......................   Passed    0.05 sec
2/6 Test #102: SomeSuite.FailOne .......................***Failed    0.07 sec
3/6 Test #103: SomeSuite.PassTwo .......................   Passed    0.03 sec
4/6 Test #104: SomeSuite.ExceptionOne ...................***Exception: SegFault    0.02 sec
5/6 Test #105: SomeSuite.TimeoutOne .....................***Timeout   30.00 sec
6/6 Test #106: SomeSuite.PassThree ......................   Passed    0.04 sec

50% tests passed, 3 tests failed out of 6

Total Test time (real) =  30.21 sec

The following tests FAILED:
	102 - SomeSuite.FailOne (Failed)
	104 - SomeSuite.ExceptionOne (SEGFAULT)
	105 - SomeSuite.TimeoutOne (Timeout)
)");

    const TRC::ParsedTests t = TRC::parseCtestOutput(log);
    ASSERT_TRUE(t.recognised);
    const QStringList names = failureNames(t);

    EXPECT_TRUE(names.contains(QStringLiteral("SomeSuite.FailOne")))
        << "failingTests = [" << names.join(", ").toStdString() << "]";
    EXPECT_TRUE(names.contains(QStringLiteral("SomeSuite.ExceptionOne")))
        << "failingTests = [" << names.join(", ").toStdString() << "]";
    EXPECT_TRUE(names.contains(QStringLiteral("SomeSuite.TimeoutOne")))
        << "failingTests = [" << names.join(", ").toStdString() << "]";

    EXPECT_FALSE(names.contains(QStringLiteral("SomeSuite.PassOne")))
        << "a passing test must never appear in failingTests; "
           "failingTests = [" << names.join(", ").toStdString() << "]";
    EXPECT_FALSE(names.contains(QStringLiteral("SomeSuite.PassTwo")))
        << "failingTests = [" << names.join(", ").toStdString() << "]";
    EXPECT_FALSE(names.contains(QStringLiteral("SomeSuite.PassThree")))
        << "failingTests = [" << names.join(", ").toStdString() << "]";
}

// INV-3 — guard: the summary counts stay correct on both fixtures above,
// independent of whether failingTests itself is populated. Must pass
// both before and after the parser is fixed.
TEST(TestResCacheParse, Inv3SummaryCountsStayCorrect) {
    const QString log1 = QStringLiteral(R"(Test project /build
1/1 Test #2711: AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes ........***Failed    0.07 sec

0% tests passed, 1 tests failed out of 1

Total Test time (real) =   0.07 sec

The following tests FAILED:
	2711 - AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes (Failed) fast features
)");
    const TRC::ParsedTests t1 = TRC::parseCtestOutput(log1);
    EXPECT_EQ(t1.passed, 0) << "passed=" << t1.passed;
    EXPECT_EQ(t1.failed, 1) << "failed=" << t1.failed;
    EXPECT_EQ(t1.total, 1) << "total=" << t1.total;

    const QString log2 = QStringLiteral(R"(Test project /build
1/6 Test #101: SomeSuite.PassOne .......................   Passed    0.05 sec
2/6 Test #102: SomeSuite.FailOne .......................***Failed    0.07 sec
3/6 Test #103: SomeSuite.PassTwo .......................   Passed    0.03 sec
4/6 Test #104: SomeSuite.ExceptionOne ...................***Exception: SegFault    0.02 sec
5/6 Test #105: SomeSuite.TimeoutOne .....................***Timeout   30.00 sec
6/6 Test #106: SomeSuite.PassThree ......................   Passed    0.04 sec

50% tests passed, 3 tests failed out of 6

Total Test time (real) =  30.21 sec

The following tests FAILED:
	102 - SomeSuite.FailOne (Failed)
	104 - SomeSuite.ExceptionOne (SEGFAULT)
	105 - SomeSuite.TimeoutOne (Timeout)
)");
    const TRC::ParsedTests t2 = TRC::parseCtestOutput(log2);
    EXPECT_EQ(t2.passed, 3) << "passed=" << t2.passed;
    EXPECT_EQ(t2.failed, 3) << "failed=" << t2.failed;
    EXPECT_EQ(t2.total, 6) << "total=" << t2.total;
}
