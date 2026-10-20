// bench_startup — ANTS-5134: launch to first paint of the real ants-terminal.
//
// Starts the built binary as a child process and times until it prints the
// first-paint marker TerminalWidget::paintEvent emits under
// ANTS_PERF_FIRST_PAINT, after which it quits. The time includes process
// start and dynamic linking, which a user waits for too.
//
// Each launch is isolated the way tools/e2e/run.sh isolates its instances: a
// throwaway XDG config, data and cache tree and its own remote-control socket,
// so it reads none of the user's settings and writes none of their state. It
// runs offscreen, with /bin/sh as the shell so a slow shell rc file is not
// counted.
//
//   runs,min_ms,median_ms
//
// ANTS_TERMINAL_BIN is baked in by CMake; the environment variable of the
// same name overrides it.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "perf_metric.h"

namespace {

constexpr int kRuns = 5;
constexpr int kTimeoutMs = 20'000;

// Milliseconds to the marker, or a negative value when it never came.
double launchOnce(const QString &bin) {
    QTemporaryDir home;
    if (!home.isValid()) return -1;
    const QDir d(home.path());
    d.mkpath(QStringLiteral("cfg/ants-terminal"));
    d.mkpath(QStringLiteral("data"));
    d.mkpath(QStringLiteral("cache"));
    // The welcome dialog would open on a first launch (ANTS-5558).
    QFile cfg(d.filePath(QStringLiteral("cfg/ants-terminal/config.json")));
    if (!cfg.open(QIODevice::WriteOnly)) return -1;
    cfg.write("{\"ui.welcome_shown\": true}\n");
    cfg.close();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), d.filePath(QStringLiteral("cfg")));
    env.insert(QStringLiteral("XDG_DATA_HOME"), d.filePath(QStringLiteral("data")));
    env.insert(QStringLiteral("XDG_CACHE_HOME"), d.filePath(QStringLiteral("cache")));
    env.insert(QStringLiteral("ANTS_REMOTE_SOCKET"), d.filePath(QStringLiteral("rc.sock")));
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    env.insert(QStringLiteral("SHELL"), QStringLiteral("/bin/sh"));
    env.insert(QStringLiteral("ANTS_PERF_FIRST_PAINT"), QStringLiteral("1"));

    QProcess p;
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.setReadChannel(QProcess::StandardError);
    QElapsedTimer t;
    t.start();
    p.start(bin, {});
    if (!p.waitForStarted(kTimeoutMs)) return -1;

    QByteArray err;
    while (t.elapsed() < kTimeoutMs) {
        if (!p.waitForReadyRead(250) && p.state() == QProcess::NotRunning)
            break;
        err += p.readAllStandardError();
        if (err.contains("ANTS_PERF_FIRST_PAINT")) {
            const double ms = t.nsecsElapsed() / 1e6;
            if (!p.waitForFinished(5000)) p.kill();
            return ms;
        }
    }
    p.kill();
    p.waitForFinished(2000);
    return -1;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QString bin = qEnvironmentVariable("ANTS_TERMINAL_BIN");
    if (bin.isEmpty()) bin = QStringLiteral(ANTS_TERMINAL_BIN);

    std::vector<double> ms;
    for (int i = 0; i < kRuns; ++i) {
        const double v = launchOnce(bin);
        if (v < 0) {
            std::fprintf(stderr, "bench_startup: run %d of %d never painted "
                                 "(binary %s)\n", i + 1, kRuns, qPrintable(bin));
            return 2;
        }
        ms.push_back(v);
    }
    std::sort(ms.begin(), ms.end());
    std::printf("runs,min_ms,median_ms\n%d,%.1f,%.1f\n", kRuns, ms.front(),
                ms[ms.size() / 2]);
    AntsPerf::reportLowerBetter("startup.first_paint_ms", ms[ms.size() / 2], "ms");
    return 0;
}
