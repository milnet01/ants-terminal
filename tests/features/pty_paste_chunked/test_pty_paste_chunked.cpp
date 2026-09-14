// A large paste reaches the shell whole — see spec.md. ANTS-5075.
// Behavioural: a real VtStream feeding a child that reads its terminal in raw
// mode, so the line discipline neither truncates nor rewrites the bytes.

#include "vtstream.h"

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <string>
#include <sys/stat.h>

#ifndef SRC_TERMINALWIDGET_IMPL_PATH
#  error "SRC_TERMINALWIDGET_IMPL_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::atomic<bool> g_writeLost{false};
QtMessageHandler g_previousHandler = nullptr;

// VtStream reports Pty::writeLost as this warning; it is the only outside view.
void captureWriteLost(QtMsgType type, const QMessageLogContext &ctx,
                      const QString &msg) {
    if (msg.contains(QLatin1String("PTY dropped"))) g_writeLost = true;
    if (g_previousHandler) g_previousHandler(type, ctx, msg);
}

// Larger than Pty's 4 MiB pending-write cap, wrapped the way performPaste wraps.
QByteArray makePaste() {
    const qsizetype size = qsizetype(5) * 1024 * 1024;
    QByteArray body;
    body.reserve(size);
    for (qsizetype i = 0; i < size; ++i) body.append(char('a' + i % 26));
    return QByteArray("\x1B[200~") + body + QByteArray("\x1B[201~");
}

void pumpUntil(int budgetMs, const std::function<bool()> &done) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

int runMain() {
    expect_reset();

    QTemporaryDir dir;
    if (!dir.isValid()) {
        expect(false, "ANTS-5075-paste-setup", "could not create QTemporaryDir");
        return expect_finish();
    }
    const QString out = dir.path() + "/received.bin";
    const QString script = dir.path() + "/child.sh";
    {
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            expect(false, "ANTS-5075-paste-setup", "could not write child.sh");
            return expect_finish();
        }
        f.write("#!/bin/sh\nstty raw -echo\nprintf READY\nexec cat > '" +
                out.toLocal8Bit() + "'\n");
    }
    ::chmod(script.toLocal8Bit().constData(), 0755);

    const QByteArray paste = makePaste();
    const QByteArray typed("TYPED-AFTER-PASTE");
    g_previousHandler = qInstallMessageHandler(captureWriteLost);
    {
        VtStream stream;
        bool ready = false;
        QObject::connect(&stream, &VtStream::batchReady,
                         [&](const VtBatchPtr &batch) {
                             if (batch->rawBytes.contains("READY")) ready = true;
                             stream.drainAck();
                         });
        const bool started = stream.start(script, dir.path(), 24, 80);
        expect(started, "ANTS-5075-paste-setup", "VtStream::start(child.sh) failed");
        if (started) {
            pumpUntil(5000, [&] { return ready; });
            expect(ready, "ANTS-5075-paste-setup",
                   "the child never put its terminal in raw mode");

            stream.writePaste(paste);
            stream.write(typed);  // a key typed while the paste is still being fed

            const qint64 want = paste.size() + typed.size();
            pumpUntil(30000, [&] { return QFileInfo(out).size() >= want; });
            QFile got(out);
            const QByteArray data =
                got.open(QIODevice::ReadOnly) ? got.readAll() : QByteArray();

            expect(data.size() == want, "ANTS-5075-paste-INV-1",
                   "received " + std::to_string(data.size()) + " of " +
                       std::to_string(want) + " bytes");
            expect(data.startsWith(paste), "ANTS-5075-paste-INV-1",
                   "the paste did not arrive byte for byte, end marker last");
            expect(data.indexOf(typed) == paste.size(), "ANTS-5075-paste-INV-3",
                   "the typed bytes did not land right after the paste");
        }
    }
    qInstallMessageHandler(g_previousHandler);
    expect(!g_writeLost.load(), "ANTS-5075-paste-INV-2",
           "Pty dropped bytes while the paste was fed");

    // INV-4 — one call hands the whole wrapped payload to VtStream::writePaste.
    const std::string body = ants_test::stripComments(ants_test::slurpFunctionBody(
        SRC_TERMINALWIDGET_IMPL_PATH, "void TerminalWidget::performPaste("));
    expect(!body.empty(), "ANTS-5075-paste-setup", "performPaste body not found");
    expect(body.find("\"writePaste\"") != std::string::npos, "ANTS-5075-paste-INV-4",
           "performPaste does not hand the paste to VtStream::writePaste");
    expect(body.find("ptyWrite(") == std::string::npos, "ANTS-5075-paste-INV-4",
           "performPaste still writes the paste as separate pieces");

    return expect_finish();
}

}  // namespace

TEST(PtyPasteChunked, Main) {
    ASSERT_EQ(0, runMain());
}
