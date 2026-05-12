// ANTS-1250 — gitwrap implementation. See gitwrap.h.

#include "gitwrap.h"

#include <QProcess>

namespace GitWrap {

Result run(const QString &workingDir, const QStringList &argv) {
    Result r;
    QProcess p;
    p.setWorkingDirectory(workingDir);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    // ANTS-1250-INV-6: argv form of QProcess::start. Never the
    // shell-interpolated single-string form. Mirrors the
    // cmdWorkspaceSearch invocation from ANTS-1248.
    p.start(QStringLiteral("git"), argv);
    if (!p.waitForStarted(500)) {
        r.started = false;
        return r;
    }
    r.started = true;

    // ANTS-1250-INV-12: 5 s wall-clock kill, 200 ms grace then SIGKILL.
    const bool finished = p.waitForFinished(kHardKillMs);
    if (!finished) {
        r.hardKilled = true;
        p.terminate();
        if (!p.waitForFinished(kKillGraceMs)) {
            p.kill();
            p.waitForFinished(kKillGraceMs);
        }
    }

    r.stdoutBytes = p.readAllStandardOutput();
    r.stderrTail  = p.readAllStandardError();
    if (r.stderrTail.size() > kStderrCapBytes) {
        r.stderrTail.truncate(kStderrCapBytes);
    }
    r.exitCode = p.exitCode();
    r.crashed  = (p.exitStatus() != QProcess::NormalExit) && !r.hardKilled;
    return r;
}

}  // namespace GitWrap
