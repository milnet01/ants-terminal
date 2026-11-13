// A screen clear clears the selection however the bytes arrive — see
// spec.md. ANTS-5075. Behavioural: starts a VtStream on a script shell.

#include "vtstream.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>

#include <string>
#include <sys/stat.h>

ANTS_TEST_SCOPE();

namespace {

struct Run {
    bool started = false;
    bool finished = false;
    int batches = 0;
    bool hint = false;
};

Run runScript(const QByteArray &body) {
    Run r;
    QTemporaryDir dir;
    if (!dir.isValid()) return r;
    const QString script = dir.path() + "/child.sh";
    {
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return r;
        f.write(body);
    }
    ::chmod(script.toLocal8Bit().constData(), 0755);

    VtStream stream;
    QObject::connect(&stream, &VtStream::batchReady,
                     [&](const VtBatchPtr &batch) {
                         ++r.batches;
                         if (batch->clearSelectionHint) r.hint = true;
                         stream.drainAck();
                     });
    QObject::connect(&stream, &VtStream::finished,
                     [&](int) { r.finished = true; });
    r.started = stream.start(script, dir.path(), 24, 80);
    if (!r.started) return r;

    QElapsedTimer clock;
    clock.start();
    while (!r.finished && clock.elapsed() < 4000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return r;
}

int runMain() {
    expect_reset();

    // INV-1 — the pause makes the two halves arrive as separate reads.
    const Run split = runScript("#!/bin/sh\n"
                                "printf '\\033[2'\n"
                                "sleep 0.4\n"
                                "printf 'J'\n"
                                "sleep 0.4\n"
                                "exit 0\n");
    expect(split.started && split.finished, "ANTS-5075-clear-setup",
           "the split-clear child did not start and finish");
    expect(split.batches >= 2, "ANTS-5075-clear-setup",
           "expected the two halves in separate batches; got " +
               std::to_string(split.batches));
    expect(split.hint, "ANTS-5075-clear-INV-1",
           "ESC[2 then J in a later read set no clearSelectionHint");

    // INV-2
    const Run plain = runScript("#!/bin/sh\nprintf 'hello\\n'\nexit 0\n");
    expect(plain.started && plain.finished, "ANTS-5075-clear-setup",
           "the plain-output child did not start and finish");
    expect(!plain.hint, "ANTS-5075-clear-INV-2",
           "plain output set clearSelectionHint");

    // INV-3 (ANTS-5784) — the grid erases on CSI ? 2 J as it does on ED 2, so
    // the selection goes with it.
    const Run decsed = runScript("#!/bin/sh\nprintf '\\033[?2J'\nsleep 0.2\nexit 0\n");
    expect(decsed.started && decsed.finished, "ANTS-5075-clear-setup",
           "the DECSED child did not start and finish");
    expect(decsed.hint, "ANTS-5075-clear-INV-3",
           "CSI ? 2 J set no clearSelectionHint");

    return expect_finish();
}

}  // namespace

TEST(VtstreamClearHint, Main) {
    ASSERT_EQ(0, runMain());
}
