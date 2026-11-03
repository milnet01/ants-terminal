// ANTS-4398 — mutation_probe engine. See mutationprobe.h.

#include "mutationprobe.h"
#include "testrescache.h"   // ANTS-4996 — the shared ctest summary matcher
#include "testrunners.h"    // ANTS-4974 — failing test names per runner

#include <QRegularExpression>

#include <utility>

namespace MutationProbe {

ApplyResult applyOne(const QString &content, const Mutation &m) {
    ApplyResult r;
    // An empty `old` matches at every position, so a naive replace would
    // splice `new` between every character. Refused as its own inert reason
    // rather than as a generic bad_args: the caller's mutation list is data,
    // and one malformed entry must not lose the other results.
    if (m.oldText.isEmpty()) {
        r.inert = Inert::OldTextEmpty;
        return r;
    }
    r.occurrences = content.count(m.oldText);
    if (r.occurrences == 0) {
        // The commonest inert case and the dangerous one: the test run that
        // follows is against UNMUTATED code, so it passes, and a caller
        // reading only the outcome concludes the suite is weak.
        r.inert = Inert::OldTextAbsent;
        return r;
    }
    if (m.newText == m.oldText) {
        r.inert = Inert::Unchanged;
        return r;
    }
    QString patched = content;
    patched.replace(m.oldText, m.newText);
    // Belt and braces: a replacement that produces identical bytes despite
    // new != old is not reachable through QString::replace, but the guarantee
    // this verb sells is "the file changed", so it is checked rather than
    // reasoned about.
    if (patched == content) {
        r.inert = Inert::Unchanged;
        return r;
    }
    r.ok = true;
    r.patched = std::move(patched);
    return r;
}

Counts parseCounts(const QString &output) {
    Counts c;

    // ctest: "97% tests passed, 2 tests failed out of 42", or on a green run
    // "100% tests passed out of 42" (ANTS-4996 — the failed clause is absent).
    if (int failed = 0, total = 0;
        TestResCache::parseCtestSummary(output, &failed, &total)) {
        c.failed = failed;
        c.passed = total - failed;
        return c;
    }

    // ANTS-5475 — vitest ("Tests  1 failed | 10 passed (11)") and jest
    // ("Tests:  2 failed, 9 passed, 11 total") print a FILE-count line first
    // ("Test Files  1 passed", "Test Suites: 1 failed"). The counts are read
    // from the `Tests` line alone, or a green run reports its file count.
    static const QRegularExpression jsTests(
        QStringLiteral(R"(^\s*Tests:?\s+(.*)$)"),
        QRegularExpression::MultilineOption);
    if (const auto jt = jsTests.match(output); jt.hasMatch()) {
        const QString line = jt.captured(1);
        static const QRegularExpression n(QStringLiteral(R"((\d+) (passed|failed))"));
        bool any = false;
        for (auto it = n.globalMatch(line); it.hasNext();) {
            const auto m = it.next();
            (m.captured(2) == QLatin1String("passed") ? c.passed : c.failed) =
                m.captured(1).toInt();
            any = true;
        }
        if (any) {
            if (c.passed < 0) c.passed = 0;
            if (c.failed < 0) c.failed = 0;
            return c;
        }
    }

    // pytest: "3 failed, 5 passed in 1.23s" — either half may be absent.
    static const QRegularExpression pyPassed(
        QStringLiteral(R"((\d+) passed)"));
    static const QRegularExpression pyFailed(
        QStringLiteral(R"((\d+) failed)"));
    const auto pp = pyPassed.match(output);
    const auto pf = pyFailed.match(output);
    if (pp.hasMatch() || pf.hasMatch()) {
        if (pp.hasMatch()) c.passed = pp.captured(1).toInt();
        if (pf.hasMatch()) c.failed = pf.captured(1).toInt();
        // A pytest run reporting only failures passed nothing, and vice
        // versa. Filling the absent half with 0 is safe HERE because the
        // summary line names every non-zero bucket — unlike the no-match case
        // below, where -1 must stand.
        if (c.passed < 0) c.passed = 0;
        if (c.failed < 0) c.failed = 0;
        return c;
    }

    // gtest: "[  PASSED  ] 5 tests." / "[  FAILED  ] 2 tests"
    static const QRegularExpression gtPassed(
        QStringLiteral(R"(\[\s*PASSED\s*\]\s*(\d+) test)"));
    static const QRegularExpression gtFailed(
        QStringLiteral(R"(\[\s*FAILED\s*\]\s*(\d+) test)"));
    const auto gp = gtPassed.match(output);
    const auto gf = gtFailed.match(output);
    if (gp.hasMatch() || gf.hasMatch()) {
        if (gp.hasMatch()) c.passed = gp.captured(1).toInt();
        if (gf.hasMatch()) c.failed = gf.captured(1).toInt();
        if (c.passed < 0) c.passed = 0;
        if (c.failed < 0) c.failed = 0;
        return c;
    }

    // ctest prints this and exits 0 when -R selects nothing (ANTS-6210).
    if (output.contains(QLatin1String("No tests were found"))) {
        c.passed = 0;
        c.failed = 0;
        return c;
    }

    // Unrecognised. Both stay -1: a run whose output could not be parsed has
    // NOT told the caller that nothing passed, and reporting 0 would be a
    // confident wrong answer of exactly the kind this verb exists to prevent.
    return c;
}

// ANTS-4401 — see mutationprobe.h. Ordered so the loudest evidence wins: a
// non-zero exit is red whatever the counts say, and only then is the absence
// of evidence separated from evidence of success.
bool budgetExhausted(qint64 elapsedMs, qint64 slowestRunMs, int budgetSec) {
    if (budgetSec <= 0)    return false;  // disabled
    if (slowestRunMs <= 0) return false;  // nothing measured yet to estimate from
    return elapsedMs + slowestRunMs > qint64(budgetSec) * 1000;
}

BaselineVerdict judgeBaseline(bool timedOut, int exitCode, const Counts &c) {
    // ANTS-4852 — pytest exits 4 (usage error) or 5 (no tests collected) when
    // it measured nothing: a mistyped path or filter. With no counts parsed
    // that is a call that never ran a test, not a red suite, and "fix the
    // suite first" pointed at the wrong thing. A runner that printed real
    // counts is still judged on them.
    if (!timedOut && (exitCode == 4 || exitCode == 5)
        && c.passed < 0 && c.failed < 0)
        return BaselineVerdict::DidNotRun;
    if (timedOut || exitCode != 0) return BaselineVerdict::NotGreen;
    if (c.passed < 0 || c.failed < 0) return BaselineVerdict::Unreadable;
    if (c.passed == 0 && c.failed == 0) return BaselineVerdict::Empty;
    // A parsed run that failed something exited 0 only if the runner lies
    // about its exit code — rare, and reported red rather than green, because
    // the count is the more specific witness.
    if (c.failed > 0) return BaselineVerdict::NotGreen;
    return BaselineVerdict::Green;
}

MutantVerdict judgeMutant(int exitCode, const Counts &c) {
    if (exitCode == 0) return MutantVerdict::Survived;
    if (c.passed < 0 || c.failed < 0) return MutantVerdict::Broken;
    return MutantVerdict::Killed;
}

namespace {

// A Python failure message: pytest's short-summary text after " - ", or an
// `E ` line. "assert ...", "AssertionError" and pytest's own "Failed:"
// (pytest.fail, DID NOT RAISE) are the test's check; any other exception
// type is a crash. pytest-timeout also reports through "Failed:", so that
// one is an error.
FailureKind classifyPython(const QString &msg) {
    if (msg.startsWith(QLatin1String("assert ")) || msg == QLatin1String("assert")
        || msg.startsWith(QLatin1String("AssertionError")))
        return FailureKind::Assertion;
    if (msg.startsWith(QLatin1String("Failed: Timeout"))) return FailureKind::Error;
    if (msg.startsWith(QLatin1String("Failed:"))) return FailureKind::Assertion;
    static const QRegularExpression exc(QStringLiteral(
        R"(^(?:[A-Za-z_]\w*\.)*[A-Za-z_]\w*(?:Error|Exception|Exit|Interrupt|Fault)(?::|$))"));
    return exc.match(msg).hasMatch() ? FailureKind::Error : FailureKind::Unknown;
}

// gtest run on its own: each "[ RUN      ] X" up to "[  FAILED  ] X (N ms)"
// is one failure. The closing list repeats the names without "(N ms)", and
// "[  FAILED  ] 2 tests, listed below:" is not a name, so neither matches.
TestResCache::ParsedTests parseGtestBlocks(const QString &output) {
    TestResCache::ParsedTests t;
    static const QRegularExpression run(QStringLiteral(R"(^\[ RUN      \] (\S+))"));
    static const QRegularExpression failed(
        QStringLiteral(R"(^\[  FAILED  \] ([^\s,]+).* \(\d+ ms\)$)"));
    const QStringList lines = output.split(QLatin1Char('\n'));
    qsizetype start = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString line = lines[i].endsWith(QLatin1Char('\r')) ? lines[i].chopped(1)
                                                                   : lines[i];
        if (run.match(line).hasMatch()) { start = i; continue; }
        const auto f = failed.match(line);
        if (!f.hasMatch() || start < 0) continue;
        if (!TestResCache::appendFailure(
                t, f.captured(1), lines.mid(start, i + 1 - start).join(QLatin1Char('\n'))))
            break;
        start = -1;
    }
    return t;
}

}  // namespace

FailureKind classifyFailure(const QString &block) {
    bool assertion = false;

    // ctest's status line: a crash, abort or timeout is never a check failing.
    static const QRegularExpression ctestCrash(QStringLiteral(
        R"(Test\s+#\d+:.*(?:\*\*\*Exception:|Subprocess aborted|\*\*\*Timeout))"));
    if (ctestCrash.match(block).hasMatch()) return FailureKind::Error;

    // gtest.
    if (block.contains(QLatin1String("C++ exception with description"))
        || block.contains(QLatin1String("Unknown C++ exception thrown")))
        return FailureKind::Error;
    static const QRegularExpression gtestAssert(QStringLiteral(R"(^\S.*:\d+: Failure\s*$)"),
                                                QRegularExpression::MultilineOption);
    if (gtestAssert.match(block).hasMatch()) assertion = true;

    // pytest: every short-summary line in the block (several, when ctest
    // wraps a whole pytest run); failing those, the first `E ` line. A pytest
    // id holds "::" or ".py", so a captured "ERROR ..." print is not one.
    static const QRegularExpression summary(
        QStringLiteral(R"(^(FAILED|ERROR) \S*(?:::|\.py)\S*(?: - (.*))?$)"),
        QRegularExpression::MultilineOption);
    bool sawSummaryMessage = false;
    for (auto it = summary.globalMatch(block); it.hasNext();) {
        const auto m = it.next();
        if (m.captured(1) == QLatin1String("ERROR")) return FailureKind::Error;
        if (!m.hasCaptured(2) || m.captured(2).isEmpty()) continue;
        sawSummaryMessage = true;
        const FailureKind k = classifyPython(m.captured(2).trimmed());
        if (k == FailureKind::Error) return k;
        if (k == FailureKind::Assertion) assertion = true;
    }
    if (!sawSummaryMessage) {
        static const QRegularExpression eLine(QStringLiteral(R"(^E\s+(\S.*)$)"),
                                              QRegularExpression::MultilineOption);
        if (const auto m = eLine.match(block); m.hasMatch()) {
            const FailureKind k = classifyPython(m.captured(1).trimmed());
            if (k == FailureKind::Error) return k;
            if (k == FailureKind::Assertion) assertion = true;
        }
    }
    return assertion ? FailureKind::Assertion : FailureKind::Unknown;
}

KillReport readKill(const QString &output) {
    TestResCache::ParsedTests t;
    if (const auto r = TestRunners::sniff(output)) t = TestRunners::parse(*r, output);
    if (t.failingTests.isEmpty()) t = parseGtestBlocks(output);
    KillReport k;
    k.truncated = t.failingTestsTruncated;
    for (const auto &f : t.failingTests) {
        k.failedTests << f.name;
        switch (classifyFailure(f.fullExcerpt)) {
            case FailureKind::Assertion: ++k.assertion; break;
            case FailureKind::Error:     ++k.error;     break;
            case FailureKind::Unknown:   ++k.unknown;   break;
        }
    }
    return k;
}

QString killedBy(const KillReport &k) {
    const int n = k.assertion + k.error + k.unknown;
    if (n == 0 || k.unknown == n) return QStringLiteral("unknown");
    if (k.assertion == n) return QStringLiteral("assertion");
    if (k.error == n) return QStringLiteral("error");
    return QStringLiteral("mixed");
}

}  // namespace MutationProbe
