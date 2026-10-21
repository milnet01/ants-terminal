// ANTS-5503 — feature-conformance test for TestRunners::sniff and
// TestRunners::parse against output captured from real runs. See spec.md
// and docs/specs/ANTS-5503-multi-runner-tests.md (INV-2..5, INV-7).

#include <gtest/gtest.h>

#include "testrunners.h"

#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace TR = TestRunners;
using TR::Runner;

namespace {

QString fixture(const char *name) {
#ifdef ANTS_SOURCE_DIR
    QFile f(QStringLiteral(ANTS_SOURCE_DIR "/tests/features/testrunners_parse/fixtures/")
            + QString::fromLatin1(name) + QStringLiteral(".txt"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
#else
    Q_UNUSED(name);
    return {};
#endif
}

QStringList names(const TestResCache::ParsedTests &t) {
    QStringList out;
    for (const auto &pf : t.failingTests) out.append(pf.name);
    return out;
}

QString excerptOf(const TestResCache::ParsedTests &t, const QString &name) {
    for (const auto &pf : t.failingTests)
        if (pf.name == name) return pf.fullExcerpt;
    return {};
}

void expectCounts(const TestResCache::ParsedTests &t, int passed, int failed,
                  int skipped) {
    EXPECT_TRUE(t.recognised);
    EXPECT_EQ(t.passed, passed);
    EXPECT_EQ(t.failed, failed);
    EXPECT_EQ(t.skipped, skipped);
    EXPECT_EQ(t.total, passed + failed + skipped);
}

// A real ctest log, from tests/features/testrescache_parse/.
const char *kCtestLog = R"(Test project /build
    Start 2711: AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes
1/2 Test #2711: AuditScopeSinceLastRun.Inv10BranchDiffNoMainBranchDemotes ........***Failed    0.07 sec
2/2 Test #2712: AuditScopeSinceLastRun.Inv11 ........   Passed    0.01 sec

50% tests passed, 1 tests failed out of 2

Total Test time (real) =   0.08 sec
)";

}  // namespace

TEST(TestRunnersParse, FixturesPresent) {
    for (const char *n : {"pytest", "cargo", "go", "jest", "vitest"})
        EXPECT_FALSE(fixture(n).isEmpty()) << n;
}

// INV-2 — ctest wins the sniff, even when its log carries a pytest-shaped line.
TEST(TestRunnersParse, Inv2CtestSniffedFirst) {
    const QString log = QString::fromUtf8(kCtestLog);
    EXPECT_EQ(TR::sniff(log), Runner::Ctest);
    QString mixed = log;
    mixed.insert(mixed.indexOf(QStringLiteral("50%")),
                 QStringLiteral("==== 3 passed in 0.10s ====\n"));
    EXPECT_EQ(TR::sniff(mixed), Runner::Ctest);
    EXPECT_EQ(TR::parse(Runner::Ctest, log).failed, 1);
}

TEST(TestRunnersParse, SniffEachRunner) {
    EXPECT_EQ(TR::sniff(fixture("pytest")), Runner::Pytest);
    EXPECT_EQ(TR::sniff(fixture("pytest_q")), Runner::Pytest);
    EXPECT_EQ(TR::sniff(fixture("pytest_none")), Runner::Pytest);
    EXPECT_EQ(TR::sniff(fixture("cargo")), Runner::Cargo);
    EXPECT_EQ(TR::sniff(fixture("go")), Runner::Go);
    EXPECT_EQ(TR::sniff(fixture("jest")), Runner::Jest);
    EXPECT_EQ(TR::sniff(fixture("jest_related_none")), Runner::Jest);
    EXPECT_EQ(TR::sniff(fixture("vitest")), Runner::Vitest);
    EXPECT_EQ(TR::sniff(fixture("vitest_none")), Runner::Vitest);
    EXPECT_FALSE(TR::sniff(fixture("npx_missing")).has_value());
    EXPECT_FALSE(TR::sniff(fixture("go_nov")).has_value());
}

// INV-3 + INV-4 — pytest: errors are failures, xfailed skipped, xpassed passed.
TEST(TestRunnersParse, Inv3Pytest) {
    const auto t = TR::parse(Runner::Pytest, fixture("pytest"));
    expectCounts(t, 4, 2, 2);  // 3 passed + 1 xpassed; 1 failed + 1 error; ...
    EXPECT_EQ(t.runner, QStringLiteral("pytest"));
    EXPECT_EQ(names(t), (QStringList{QStringLiteral("test_sample.py::test_fail"),
                                     QStringLiteral("test_sample.py::test_error")}));
    EXPECT_TRUE(excerptOf(t, QStringLiteral("test_sample.py::test_fail"))
                    .contains(QStringLiteral("x should be four")));
    EXPECT_TRUE(excerptOf(t, QStringLiteral("test_sample.py::test_error"))
                    .contains(QStringLiteral("fixture exploded")));
    EXPECT_EQ(t.durationMs, 20);
    // -q drops the `=` rules and the collected line; counts are unchanged.
    const auto q = TR::parse(Runner::Pytest, fixture("pytest_q"));
    expectCounts(q, 4, 2, 2);
    EXPECT_EQ(q.collected, -1);
}

// INV-3 + INV-4 — cargo: summed over every `test result:` line, ignored skipped.
TEST(TestRunnersParse, Inv4CargoSumsEveryBinary) {
    const auto t = TR::parse(Runner::Cargo, fixture("cargo"));
    expectCounts(t, 2, 2, 1);
    EXPECT_EQ(names(t), (QStringList{QStringLiteral("tests::breaks"),
                                     QStringLiteral("int_fail")}));
    EXPECT_TRUE(excerptOf(t, QStringLiteral("tests::breaks"))
                    .contains(QStringLiteral("math is off")));
    EXPECT_TRUE(excerptOf(t, QStringLiteral("int_fail"))
                    .contains(QStringLiteral("integration went wrong")));
}

// INV-3 + INV-5 — go: top-level tests only; a failing subtest fails its parent.
TEST(TestRunnersParse, Inv5GoCountsTopLevelOnly) {
    const auto t = TR::parse(Runner::Go, fixture("go"));
    expectCounts(t, 2, 2, 1);
    EXPECT_EQ(names(t), (QStringList{QStringLiteral("TestFail"),
                                     QStringLiteral("TestParent")}));
    EXPECT_TRUE(excerptOf(t, QStringLiteral("TestParent"))
                    .contains(QStringLiteral("child broke")));
    EXPECT_EQ(t.durationMs, 4);
}

// INV-5 — without -v (and with no test at all) go output is not recognised,
// even when a failing run prints `--- FAIL:` lines.
TEST(TestRunnersParse, Inv5GoWithoutVerboseNotRecognised) {
    EXPECT_FALSE(TR::parse(Runner::Go, fixture("go_nov")).recognised);
    EXPECT_FALSE(TR::parse(Runner::Go, fixture("go_none")).recognised);
}

// A panicking test, and a timed-out one that prints no `--- FAIL:` line: the
// failed package still counts as a failure, so the run cannot read as green.
TEST(TestRunnersParse, GoPanicAndTimeoutFail) {
    const auto p = TR::parse(Runner::Go, fixture("go_panic"));
    expectCounts(p, 1, 1, 0);
    EXPECT_TRUE(excerptOf(p, QStringLiteral("TestBoom"))
                    .contains(QStringLiteral("nil map")));
    const auto t = TR::parse(Runner::Go, fixture("go_timeout"));
    expectCounts(t, 1, 1, 0);
    EXPECT_EQ(names(t), QStringList{QStringLiteral("example.com/gt")});
    EXPECT_TRUE(excerptOf(t, QStringLiteral("example.com/gt"))
                    .contains(QStringLiteral("timed out")));
}

// INV-3 — jest: todo counts as skipped, so total matches jest's own total.
TEST(TestRunnersParse, Inv3Jest) {
    const auto t = TR::parse(Runner::Jest, fixture("jest"));
    expectCounts(t, 1, 1, 2);
    EXPECT_EQ(names(t), QStringList{QString::fromUtf8("math › breaks")});
    EXPECT_TRUE(excerptOf(t, QString::fromUtf8("math › breaks"))
                    .contains(QStringLiteral("Received: 3")));
    EXPECT_EQ(t.durationMs, 337);
    const auto none = TR::parse(Runner::Jest, fixture("jest_related_none"));
    EXPECT_TRUE(none.recognised);
    EXPECT_EQ(none.total, 0);
}

// INV-3 — vitest: the indented `Tests` line, todo as skipped.
TEST(TestRunnersParse, Inv3Vitest) {
    const auto t = TR::parse(Runner::Vitest, fixture("vitest"));
    expectCounts(t, 1, 1, 2);
    EXPECT_EQ(names(t), QStringList{QStringLiteral("math.test.js > math > breaks")});
    EXPECT_TRUE(excerptOf(t, QStringLiteral("math.test.js > math > breaks"))
                    .contains(QStringLiteral("expected 3 to be 4")));
    EXPECT_EQ(t.durationMs, 211);
    expectCounts(TR::parse(Runner::Vitest, fixture("vitest_related")), 1, 1, 2);
    const auto none = TR::parse(Runner::Vitest, fixture("vitest_none"));
    EXPECT_TRUE(none.recognised);
    EXPECT_EQ(none.total, 0);
}

// INV-7 — `collected` is what pytest printed, never derived.
TEST(TestRunnersParse, Inv7CollectedOnlyWhenPrinted) {
    const auto d = TR::parse(Runner::Pytest, fixture("pytest_desel"));
    EXPECT_EQ(d.collected, 7);
    expectCounts(d, 1, 2, 2);  // deselected is not counted
    const auto none = TR::parse(Runner::Pytest, fixture("pytest_none"));
    EXPECT_TRUE(none.recognised);
    EXPECT_EQ(none.total, 0);
    EXPECT_EQ(none.collected, 0);
    const QJsonObject wire =
        TestResCache::toJsonWire(TR::parse(Runner::Jest, fixture("jest")));
    EXPECT_FALSE(wire.contains(QStringLiteral("collected")));
    EXPECT_EQ(TestResCache::toJsonWire(d).value(QStringLiteral("collected")).toInt(), 7);
}

// § 2.3 — output of another runner is not recognised as this one.
TEST(TestRunnersParse, WrongRunnerNotRecognised) {
    EXPECT_FALSE(TR::parse(Runner::Cargo, fixture("jest")).recognised);
    EXPECT_FALSE(TR::parse(Runner::Pytest, fixture("go")).recognised);
}
