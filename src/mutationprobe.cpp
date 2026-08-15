// ANTS-4398 — mutation_probe engine. See mutationprobe.h.

#include "mutationprobe.h"

#include <QRegularExpression>

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
    r.patched = patched;
    return r;
}

Counts parseCounts(const QString &output) {
    Counts c;

    // ctest: "100% tests passed, 0 tests failed out of 42"
    static const QRegularExpression ctestRe(
        QStringLiteral(R"((\d+) tests failed out of (\d+))"));
    if (const auto m = ctestRe.match(output); m.hasMatch()) {
        c.failed = m.captured(1).toInt();
        c.passed = m.captured(2).toInt() - c.failed;
        return c;
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

    // Unrecognised. Both stay -1: a run whose output could not be parsed has
    // NOT told the caller that nothing passed, and reporting 0 would be a
    // confident wrong answer of exactly the kind this verb exists to prevent.
    return c;
}

// ANTS-4401 — see mutationprobe.h. Ordered so the loudest evidence wins: a
// non-zero exit is red whatever the counts say, and only then is the absence
// of evidence separated from evidence of success.
BaselineVerdict judgeBaseline(bool timedOut, int exitCode, const Counts &c) {
    if (timedOut || exitCode != 0) return BaselineVerdict::NotGreen;
    if (c.passed < 0 || c.failed < 0) return BaselineVerdict::Unreadable;
    if (c.passed == 0 && c.failed == 0) return BaselineVerdict::Empty;
    // A parsed run that failed something exited 0 only if the runner lies
    // about its exit code — rare, and reported red rather than green, because
    // the count is the more specific witness.
    if (c.failed > 0) return BaselineVerdict::NotGreen;
    return BaselineVerdict::Green;
}

}  // namespace MutationProbe
