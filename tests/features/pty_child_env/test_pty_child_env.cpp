// The shell receives the terminal's whole environment — see spec.md.
// ANTS-5075. Behavioural: starts a real child and reads what it printed.

#include "ptyhandler.h"

#include "../../_support/expect.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>

#include <string>
#include <sys/stat.h>

ANTS_TEST_SCOPE();

namespace {

// More than the fixed table Pty::start used to copy the environment into.
constexpr int kPadEntries = 700;

int runMain() {
    expect_reset();

    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "ANTS-5075-setup", "could not create QTemporaryDir");
        return expect_finish();
    }
    const QString script = dir.path() + "/print_env.sh";
    {
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            expect(false, "ANTS-5075-setup", "could not write print_env.sh");
            return expect_finish();
        }
        f.write("#!/bin/sh\n"
                "printf 'S=%s T=%s P=%s\\n' \"$ANTS_PTYENV_SENTINEL\" "
                "\"$TERM\" \"$TERM_PROGRAM\"\n"
                "exit 0\n");
    }
    ::chmod(script.toLocal8Bit().constData(), 0755);

    // setenv appends a new name at the end of environ, so the sentinel lands
    // after every pad entry.
    for (int i = 0; i < kPadEntries; ++i)
        qputenv(QByteArray("ANTS_PTYENV_PAD_") + QByteArray::number(i), "x");
    qputenv("ANTS_PTYENV_SENTINEL", "reached");

    QByteArray output;
    bool finished = false;
    {
        Pty pty;
        QObject::connect(&pty, &Pty::dataReceived,
                         [&](const QByteArray &d) { output += d; });
        QObject::connect(&pty, &Pty::finished, [&](int) { finished = true; });
        const bool started = pty.start(script);

        // The child's environment is fixed at fork, so the test process can
        // drop the padding now.
        for (int i = 0; i < kPadEntries; ++i)
            qunsetenv(QByteArray("ANTS_PTYENV_PAD_") + QByteArray::number(i));
        qunsetenv("ANTS_PTYENV_SENTINEL");

        if (!started) {
            expect(false, "ANTS-5075-setup", "Pty::start(print_env.sh) failed");
            return expect_finish();
        }
        QElapsedTimer clock;
        clock.start();
        while (!finished && clock.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }

    const std::string out = output.toStdString();
    expect(output.contains("S=reached"), "ANTS-5075-INV-1",
           "the sentinel set after " + std::to_string(kPadEntries) +
           " other entries did not reach the child; it printed: " + out);
    expect(output.contains("T=xterm-256color") &&
               output.contains("P=AntsTerminal"),
           "ANTS-5075-INV-2",
           "the TERM / TERM_PROGRAM overrides were not applied; child printed: " +
               out);
    return expect_finish();
}

}  // namespace

TEST(PtyChildEnv, Main) {
    ASSERT_EQ(0, runMain());
}
