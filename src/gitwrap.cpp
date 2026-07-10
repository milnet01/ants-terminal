// ANTS-1250 — gitwrap implementation. See gitwrap.h.

#include "gitwrap.h"

#include <QProcess>
#include <QRegularExpression>

namespace GitWrap {

Result run(const QString &workingDir, const QStringList &argv,
           int maxStdoutBytes) {
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
    // ANTS-1839 — bound stdout like stderr so an unexpectedly large output
    // (e.g. a future `git diff` caller) can't feed an unbounded blob into a
    // JSON envelope. stdoutTruncated lets the caller flag the partial read.
    if (maxStdoutBytes >= 0 && r.stdoutBytes.size() > maxStdoutBytes) {
        r.stdoutBytes.truncate(maxStdoutBytes);
        r.stdoutTruncated = true;
    }
    r.stderrTail  = p.readAllStandardError();
    if (r.stderrTail.size() > kStderrCapBytes) {
        r.stderrTail.truncate(kStderrCapBytes);
    }
    r.exitCode = p.exitCode();
    r.crashed  = (p.exitStatus() != QProcess::NormalExit) && !r.hardKilled;
    return r;
}

QVector<DiffFile> parseDiffHunks(const QByteArray &unifiedDiff,
                                 bool includeLines) {
    QVector<DiffFile> out;
    // "@@ -oldStart[,oldCount] +newStart[,newCount] @@ [section]"
    static const QRegularExpression hunkRe(
        QStringLiteral("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@"));

    DiffFile cur;
    bool haveFile = false;
    auto flush = [&]() {
        // Emit only files that carry a content hunk — a pure rename / mode
        // change has a `diff --git` header but no `@@`, and numstat mode
        // already reports those.
        if (haveFile && !cur.path.isEmpty() && !cur.hunks.isEmpty())
            out.append(cur);
        cur = DiffFile();
        haveFile = false;
    };

    const QList<QByteArray> raw = unifiedDiff.split('\n');
    for (const QByteArray &lb : raw) {
        const QString line = QString::fromUtf8(lb);
        if (line.startsWith(QLatin1String("diff --git "))) {
            flush();
            haveFile = true;
            continue;
        }
        if (!haveFile) continue;
        // The `--- ` / `+++ ` path headers precede the first `@@`; once a hunk
        // has opened, a line starting `+++ ` / `--- ` is body content (an
        // added / removed line whose text begins with `+` / `-`), so gate the
        // path parse on "no hunk yet" to resolve that unified-diff ambiguity.
        if (cur.hunks.isEmpty()
            && line.startsWith(QLatin1String("--- "))) {
            QString p = line.mid(4);
            if (p.startsWith(QLatin1String("a/"))) p = p.mid(2);
            if (p != QLatin1String("/dev/null")) cur.path = p;
            continue;
        }
        if (cur.hunks.isEmpty()
            && line.startsWith(QLatin1String("+++ "))) {
            QString p = line.mid(4);
            if (p.startsWith(QLatin1String("b/"))) p = p.mid(2);
            if (p != QLatin1String("/dev/null")) cur.path = p;
            continue;
        }
        if (line.startsWith(QLatin1String("@@ "))) {
            const QRegularExpressionMatch m = hunkRe.match(line);
            if (!m.hasMatch()) continue;
            DiffHunk h;
            h.header   = line;
            h.oldStart = m.captured(1).toInt();
            h.oldCount = m.captured(2).isEmpty() ? 1 : m.captured(2).toInt();
            h.newStart = m.captured(3).toInt();
            h.newCount = m.captured(4).isEmpty() ? 1 : m.captured(4).toInt();
            cur.hunks.append(h);
            continue;
        }
        if (includeLines && !cur.hunks.isEmpty()
            && (line.startsWith(QLatin1Char(' '))
                || line.startsWith(QLatin1Char('+'))
                || line.startsWith(QLatin1Char('-'))
                || line.startsWith(QLatin1Char('\\')))) {  // "\ No newline…"
            cur.hunks.last().lines.append(line);
        }
    }
    flush();
    return out;
}

}  // namespace GitWrap
