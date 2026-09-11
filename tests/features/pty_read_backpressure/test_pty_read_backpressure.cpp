// PTY read back-pressure — see spec.md. ANTS-5026.
//
// Why this exists: Pty::onReadReady reads in a loop until EAGAIN and never
// checks, inside that loop, whether the read notifier was just disabled —
// so a pause requested from inside a dataReceived handler (exactly what
// VtStream does when too many parse batches are in flight) only takes
// effect after the loop drains whatever the kernel already had queued. The
// same loop shape lets a later setReadEnabled(true) after EOF re-enter the
// EOF branch a second time and reap an unrelated process via a bare
// waitpid(-1, ...). This is a behavioural test, not a source-grep: the
// claim is about what arrives on Pty's signals and what waitpid actually
// reaps, which only a real child process and a real pumped event loop can
// show (spec.md "Why behavioural").

#include "ptyhandler.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

ANTS_TEST_SCOPE();

namespace {

// Writes `body` to `dir`/`name`, marks it executable, and returns its
// absolute path. Empty string on failure.
QString writeExecutableScript(QTemporaryDir &dir, const QString &name,
                               const QByteArray &body) {
    const QString path = dir.path() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write(body);
    f.close();
    const QByteArray pathBytes = path.toLocal8Bit();
    if (::chmod(pathBytes.constData(), 0755) != 0) return QString();
    return path;
}

// Pumps the calling thread's event loop for up to `budgetMs`, in short
// slices so a signal delivered mid-window is observed promptly.
void pumpFor(int budgetMs) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < budgetMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
}

// INV-1 — a pause requested from inside the first dataReceived handler must
// stop delivery before a second chunk arrives.
//
// Determinism: reads are disabled BEFORE the flooding child ("exec yes")
// gets to write anything, so nothing drains while the child fills the
// kernel PTY buffer and blocks on write() during the bounded wait below.
// Re-enabling reads then hands onReadReady a backlog far larger than one
// read() call returns — a Linux PTY master read returns at most a few KB
// per call while tens of KB sit queued — so the unfixed loop, which never
// re-checks the notifier once it starts draining, emits several chunks
// before the caller's pause request has any effect.
void checkPauseHonouredAtOnce() {
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "ANTS-5026-INV-1-setup", "could not create QTemporaryDir");
        return;
    }
    const QString script =
        writeExecutableScript(dir, "flood.sh", "#!/bin/sh\nexec yes\n");
    if (script.isEmpty()) {
        expect(false, "ANTS-5026-INV-1-setup", "could not write flood.sh");
        return;
    }

    Pty pty;
    int chunkCount = 0;
    bool pauseRequested = false;
    QObject::connect(&pty, &Pty::dataReceived,
                      [&](const QByteArray &) {
                          ++chunkCount;
                          if (!pauseRequested) {
                              pauseRequested = true;
                              pty.setReadEnabled(false);
                          }
                      });

    if (!pty.start(script)) {
        expect(false, "ANTS-5026-INV-1-setup",
               "Pty::start(flood.sh) failed — cannot exercise the read loop");
        return;
    }

    // Disable reads immediately, before the event loop has had any chance
    // to deliver the notifier once. No processEvents() call happens between
    // start() and this line.
    pty.setReadEnabled(false);

    // Let the flooding child fill the kernel PTY buffer and block on
    // write(). Bounded, not polling on a condition — there is nothing to
    // observe yet since reads are disabled.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    chunkCount = 0;
    pauseRequested = false;
    pty.setReadEnabled(true);
    pumpFor(600);

    expect(pauseRequested, "ANTS-5026-INV-1-setup",
           "no dataReceived arrived after re-enabling reads on a flooding "
           "child — cannot exercise the pause request");
    expect(chunkCount == 1, "ANTS-5026-INV-1",
           "expected exactly 1 dataReceived chunk after the handler paused "
           "on the first one (kernel buffer pre-filled by a blocked `exec "
           "yes`); got " + std::to_string(chunkCount) +
           " — the read loop kept draining after setReadEnabled(false) was "
           "called from inside the handler");
}

// INV-2 — finished must fire at most once, even if reads are re-enabled
// after EOF has already been processed.
void checkNoDoubleFinishedAfterEof() {
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "ANTS-5026-INV-2-setup", "could not create QTemporaryDir");
        return;
    }
    const QString script =
        writeExecutableScript(dir, "quick_exit.sh", "#!/bin/sh\nexit 0\n");
    if (script.isEmpty()) {
        expect(false, "ANTS-5026-INV-2-setup", "could not write quick_exit.sh");
        return;
    }

    Pty pty;
    int finishedCount = 0;
    QObject::connect(&pty, &Pty::finished,
                      [&](int) { ++finishedCount; });

    if (!pty.start(script)) {
        expect(false, "ANTS-5026-INV-2-setup",
               "Pty::start(quick_exit.sh) failed — cannot exercise EOF");
        return;
    }

    {
        QElapsedTimer clock;
        clock.start();
        while (finishedCount == 0 && clock.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }
    if (finishedCount != 1) {
        expect(false, "ANTS-5026-INV-2-setup",
               "expected exactly 1 finished() before ever touching "
               "setReadEnabled — got " + std::to_string(finishedCount) +
               "; broken test setup, not the defect this test targets");
        return;
    }

    // The notifier is level-triggered and the master fd stays open after
    // EOF (only the destructor closes it) — re-enabling reads here is
    // exactly what VtStream::drainAck does unconditionally on every ack,
    // including one that lands after EOF.
    pty.setReadEnabled(true);
    pumpFor(500);

    expect(finishedCount == 1, "ANTS-5026-INV-2",
           "expected finished() to fire exactly once even after "
           "re-enabling reads post-EOF; got " + std::to_string(finishedCount) +
           " — the read loop re-entered the EOF branch a second time and "
           "emitted finished() again for the same child");
}

// INV-3 — a reap triggered by the read loop after EOF must never call
// waitpid with "any child" semantics, or it can steal an unrelated zombie.
void checkNoZombieStealAfterEof() {
    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "ANTS-5026-INV-3-setup", "could not create QTemporaryDir");
        return;
    }
    const QString script =
        writeExecutableScript(dir, "quick_exit2.sh", "#!/bin/sh\nexit 0\n");
    if (script.isEmpty()) {
        expect(false, "ANTS-5026-INV-3-setup", "could not write quick_exit2.sh");
        return;
    }

    Pty pty;
    int finishedCount = 0;
    QObject::connect(&pty, &Pty::finished,
                      [&](int) { ++finishedCount; });

    if (!pty.start(script)) {
        expect(false, "ANTS-5026-INV-3-setup",
               "Pty::start(quick_exit2.sh) failed — cannot exercise EOF");
        return;
    }

    {
        QElapsedTimer clock;
        clock.start();
        while (finishedCount == 0 && clock.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }
    if (finishedCount != 1) {
        expect(false, "ANTS-5026-INV-3-setup",
               "expected exactly 1 finished() before forking the unrelated "
               "child — got " + std::to_string(finishedCount) +
               "; broken test setup, not the defect this test targets");
        return;
    }

    // A child that belongs to THIS test process, not to the Pty at all.
    // It calls nothing but _exit(0) — no Qt, no allocation, no stdio —
    // so it is safe to run in the forked copy of a threaded process.
    const pid_t unrelatedPid = ::fork();
    if (unrelatedPid == 0) {
        ::_exit(0);
    }
    if (unrelatedPid < 0) {
        expect(false, "ANTS-5026-INV-3-setup",
               std::string("fork() failed: ") + std::strerror(errno));
        return;
    }

    // Re-enable reads on the already-finished Pty, twice. If the read loop
    // re-enters its EOF branch (INV-2's defect), it calls waitpid with the
    // Pty's own childPid. The first EOF pass leaves that pid set when its
    // WNOHANG waitpid ran before the child finished exiting, so one extra
    // pass may only reap the Pty's own child. By the second extra pass the
    // pid is -1, i.e. "any child", whichever way the first pass went.
    for (int pass = 0; pass < 2; ++pass) {
        pty.setReadEnabled(true);
        pumpFor(300);
    }

    int status = 0;
    errno = 0;
    pid_t reapedByTest = -1;
    {
        QElapsedTimer clock;
        clock.start();
        do {
            reapedByTest = ::waitpid(unrelatedPid, &status, WNOHANG);
        } while (reapedByTest == 0 && clock.elapsed() < 500);
    }
    const int reapErrno = errno;

    expect(reapedByTest == unrelatedPid, "ANTS-5026-INV-3",
           "expected this test's own waitpid(" + std::to_string(unrelatedPid) +
           ", WNOHANG) to reap the unrelated child it forked; got return " +
           std::to_string(reapedByTest) + " (errno=" + std::to_string(reapErrno) +
           " " + std::strerror(reapErrno) + ") — the Pty's read loop already "
           "reaped it via a waitpid(-1, ...) triggered by re-enabling reads "
           "after EOF");

    // Leak no zombie regardless of which side reaped it: if our own
    // waitpid above did not consume it, try once more (harmless if it is
    // already gone — WNOHANG then returns -1/ECHILD).
    if (reapedByTest != unrelatedPid) {
        ::waitpid(unrelatedPid, nullptr, WNOHANG);
    }
}

int runMain() {
    expect_reset();
    checkPauseHonouredAtOnce();
    checkNoDoubleFinishedAfterEof();
    checkNoZombieStealAfterEof();
    return expect_finish();
}

}  // namespace

TEST(PtyReadBackpressure, Main) {
    ASSERT_EQ(0, runMain());
}
