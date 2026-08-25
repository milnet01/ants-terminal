// ANTS-4651 — the budget a test fixture gives a spawned helper process, and
// the diagnosis when it does not finish in time.
//
// WHY THIS EXISTS. Twelve fixtures across eight feature directories carried a
// hard `p.waitForFinished(5000)` around a `git` invocation. On 2026-08-25 run
// 32819126506 two of them failed in build-asan at 5.79 s and 5.58 s — inside
// FIXTURE SETUP, on `initGitProject`, with the useless message
// `Value of: initGitProject(tmp.path())  Actual: false`. The same commit passed
// both in build-test at 0.04 s each, and the ten runs before it were green.
//
// That is the defect CMakeLists.txt already names one layer up, where ctest's
// own per-test TIMEOUT was raised from 10 s to 60 s on 2026-07-31: a HANG
// DETECTOR, not a performance budget, and calibrated against Release only.
// A five-second wait for a subprocess is the same mistake inside the test —
// `git` is a separate, unsanitised process, so what a sanitizer run costs here
// is contention rather than git's own runtime, and the number was picked on a
// quiet Release box.
//
// SO THE BUDGET IS NOT MERELY BIGGER. Three things change:
//
//  * It is ONE named constant rather than twelve literals, so the next
//    calibration happens once.
//  * It is overridable by the environment, because the right value is a
//    property of the machine and not of the test.
//  * It SAYS WHAT HAPPENED. The old message could not distinguish "git is not
//    installed" from "git was slow", which are opposite repairs — and it named
//    neither the command nor the budget it had blown.
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace ants_test {

// The default. Four times the 5.79 s that failed, and well inside the 60 s
// ctest TIMEOUT the bundles carry — that TIMEOUT is the layer that terminates
// a genuine hang, and this one must not race it, or the hang is reported by
// the outer killer with a worse message than the inner one would have given.
inline constexpr int kDefaultSpawnTimeoutMs = 20000;
// A junk override must not silently become 0 — that budget fails every fixture
// instantly and reads exactly like the code being broken.
inline constexpr int kMinSpawnTimeoutMs = 1000;
inline constexpr int kMaxSpawnTimeoutMs = 600000;

// Milliseconds a spawned helper gets to finish. Overridden by
// ANTS_TEST_SPAWN_TIMEOUT_MS; anything unparseable or out of range falls back
// to the default rather than to zero.
inline int spawnTimeoutMs() {
    const QByteArray raw = qgetenv("ANTS_TEST_SPAWN_TIMEOUT_MS");
    if (raw.isEmpty()) return kDefaultSpawnTimeoutMs;
    bool      ok = false;
    const int v  = raw.toInt(&ok);
    if (!ok || v < kMinSpawnTimeoutMs || v > kMaxSpawnTimeoutMs)
        return kDefaultSpawnTimeoutMs;
    return v;
}

// Wait for `p`, killing it on expiry. True iff it finished within the budget.
// `why` (optional) receives a one-line diagnosis; the same line goes to stderr,
// where ctest's --output-on-failure puts it directly above the assertion.
//
// The two failures are reported DIFFERENTLY on purpose. "Could not be started"
// and "did not finish" have opposite repairs — install the tool, versus give it
// more room — and the boolean this replaces could not tell them apart, so the
// build-asan red of 2026-08-25 read as `git` being absent on a runner that
// plainly had it.
inline bool waitForHelper(QProcess &p, QString *why = nullptr) {
    const int    budget = spawnTimeoutMs();
    QElapsedTimer t;
    t.start();
    if (p.waitForFinished(budget)) return true;

    const QString cmd = p.program() + QLatin1Char(' ') +
                        p.arguments().join(QLatin1Char(' '));
    QString msg;
    if (p.error() == QProcess::FailedToStart) {
        // No budget was consumed: Qt knows this at once. Saying so is what
        // stops a reader hunting for a slow machine.
        msg = QStringLiteral("[ants-test] helper could not be started: %1 "
                             "(is it installed and on PATH?)").arg(cmd);
    } else {
        const qint64 ms = t.elapsed();
        p.kill();
        p.waitForFinished(500);   // drain the kill; deliberately short
        msg = QStringLiteral("[ants-test] helper did not finish: %1 — killed "
                             "after %2 ms against a %3 ms budget (raise it with "
                             "ANTS_TEST_SPAWN_TIMEOUT_MS)")
                  .arg(cmd).arg(ms).arg(budget);
    }
    if (why) *why = msg;
    std::fprintf(stderr, "%s\n", msg.toLocal8Bit().constData());
    return false;
}

}  // namespace ants_test
