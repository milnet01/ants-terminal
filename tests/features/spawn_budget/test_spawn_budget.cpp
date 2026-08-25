// ANTS-4651 — the spawned-helper budget shared by every fixture that runs a
// subprocess. See spec.md for the invariant map, and tests/support/testspawn.h
// for why the old per-fixture 5000 ms literal was the wrong shape.

#include "support/testspawn.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QString>

#include <cstdlib>

#include <gtest/gtest.h>

namespace {

// Set/clear ANTS_TEST_SPAWN_TIMEOUT_MS around one check and put it back, so a
// sibling test in the same bundle cannot inherit it.
struct EnvGuard {
    QByteArray had;
    bool       wasSet;
    explicit EnvGuard(const char *value)
        : had(qgetenv("ANTS_TEST_SPAWN_TIMEOUT_MS")),
          wasSet(qEnvironmentVariableIsSet("ANTS_TEST_SPAWN_TIMEOUT_MS")) {
        if (value) qputenv("ANTS_TEST_SPAWN_TIMEOUT_MS", QByteArray(value));
        else       qunsetenv("ANTS_TEST_SPAWN_TIMEOUT_MS");
    }
    ~EnvGuard() {
        if (wasSet) qputenv("ANTS_TEST_SPAWN_TIMEOUT_MS", had);
        else        qunsetenv("ANTS_TEST_SPAWN_TIMEOUT_MS");
    }
};

}  // namespace

// INV-1 — the budget is one value, generous by default, and env-overridable.
//
// The default has to clear the measured failure by a wide margin (5.5 s
// observed against a 5 s budget) while staying well inside the bundle's own
// 60 s ctest TIMEOUT, which is the layer that terminates a genuine hang.
TEST(SpawnBudget, Inv1DefaultIsGenerousAndOverridable) {
    {
        EnvGuard g(nullptr);
        const int d = ants_test::spawnTimeoutMs();
        EXPECT_GE(d, 15000)
            << "ANTS-4651: 5000 ms was calibrated on a quiet Release box and "
               "was blown at 5.79 s by a sanitizer runner merely spawning git";
        EXPECT_LE(d, 45000)
            << "ANTS-4651: it must stay well inside the bundle's 60 s ctest "
               "TIMEOUT, or the hang detector one layer up kills the test "
               "first and with a worse message";
    }
    {
        EnvGuard g("31000");
        EXPECT_EQ(ants_test::spawnTimeoutMs(), 31000)
            << "ANTS-4651: the right value is a property of the machine, so "
               "the machine has to be able to say so";
    }
}

// INV-2 — a malformed or absurd override falls back rather than to zero.
//
// The dangerous failure is a budget of 0: every helper then "times out"
// instantly and every fixture fails, which reads as the code being broken.
TEST(SpawnBudget, Inv2MalformedOverrideFallsBackNotToZero) {
    for (const char *bad : {"", "abc", "0", "-1", "99999999"}) {
        EnvGuard g(bad);
        const int v = ants_test::spawnTimeoutMs();
        EXPECT_GE(v, 1000)
            << "ANTS-4651: a junk override must never produce a budget that "
               "fails every fixture instantly — value was " << bad;
        EXPECT_LE(v, 600000)
            << "ANTS-4651: …nor one that lets a genuine hang run past the "
               "ctest TIMEOUT — value was " << bad;
    }
}

// INV-3 — a helper that finishes is not waited out.
TEST(SpawnBudget, Inv3FastHelperReturnsPromptly) {
    QProcess p;
    p.start(QStringLiteral("/bin/true"), {});
    QElapsedTimer t;
    t.start();
    QString why;
    EXPECT_TRUE(ants_test::waitForHelper(p, &why))
        << "ANTS-4651: /bin/true finishes at once — why: " << qPrintable(why);
    EXPECT_TRUE(why.isEmpty())
        << "ANTS-4651: a success writes no diagnosis: " << qPrintable(why);
    EXPECT_LT(t.elapsed(), 10000)
        << "ANTS-4651: the budget is a ceiling, not a sleep";
}

// INV-4 — a helper that overruns is killed, reported, and names the budget.
//
// The old message was `Value of: initGitProject(...)  Actual: false`, which
// says nothing about which command, how long, or what the limit was.
TEST(SpawnBudget, Inv4OverrunIsKilledAndDiagnosed) {
    EnvGuard g("1000");
    QProcess p;
    p.start(QStringLiteral("/bin/sleep"), {QStringLiteral("30")});
    QElapsedTimer t;
    t.start();
    QString why;
    EXPECT_FALSE(ants_test::waitForHelper(p, &why))
        << "ANTS-4651: a 30 s sleep cannot finish inside a 1 s budget";
    EXPECT_LT(t.elapsed(), 15000)
        << "ANTS-4651: it must KILL the overrunning helper, not wait it out";
    EXPECT_TRUE(why.contains(QStringLiteral("sleep")))
        << "ANTS-4651: the diagnosis names the command: " << qPrintable(why);
    EXPECT_TRUE(why.contains(QStringLiteral("1000")))
        << "ANTS-4651: …and the budget it blew: " << qPrintable(why);
    EXPECT_EQ(p.state(), QProcess::NotRunning)
        << "ANTS-4651: the helper must not be left running";
}

// INV-5 — "could not start" is a DIFFERENT answer from "took too long".
//
// This is the half that cost the diagnosis: the two have opposite repairs —
// install the tool, versus give it more room — and the old boolean could not
// tell them apart. It must also not burn the budget: a program that is not
// there fails at once.
TEST(SpawnBudget, Inv5FailedToStartIsNotATimeout) {
    EnvGuard g("20000");
    QProcess p;
    p.start(QStringLiteral("ants-no-such-helper-binary"), {});
    QElapsedTimer t;
    t.start();
    QString why;
    EXPECT_FALSE(ants_test::waitForHelper(p, &why));
    EXPECT_LT(t.elapsed(), 5000)
        << "ANTS-4651: a missing program is known immediately — it must not "
           "sit out the whole budget";
    EXPECT_TRUE(why.contains(QStringLiteral("could not be started")))
        << "ANTS-4651: the two causes must read differently, or the reader is "
           "sent to the wrong repair: " << qPrintable(why);
    EXPECT_FALSE(why.contains(QStringLiteral("did not finish")))
        << "ANTS-4651: …and must not ALSO read as a timeout: "
        << qPrintable(why);
}
