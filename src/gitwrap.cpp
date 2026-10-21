// ANTS-1250 — gitwrap implementation. See gitwrap.h.

#include "gitwrap.h"
#include "hostexec.h"   // ANTS-5527 — tools run on the host inside a Flatpak

#include <QProcess>
#include <QRegularExpression>

namespace GitWrap {

Result run(const QString &workingDir, const QStringList &argv,
           int maxStdoutBytes) {
    Result r;
    QProcess p;
    p.setWorkingDirectory(workingDir);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.setProcessEnvironment(readOnlyEnvironment());   // ANTS-4999
    // ANTS-1250-INV-6: argv form of QProcess::start. Never the
    // shell-interpolated single-string form. Mirrors the
    // cmdWorkspaceSearch invocation from ANTS-1248.
    HostExec::start(p, QStringLiteral("git"), argv);
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
        // ANTS-5111 — a merge-conflict combined diff (`diff --cc` /
        // `diff --combined`) starts a new file too. Its `@@@` hunks are not
        // two-way hunks this parser understands, so the section is skipped;
        // before, its lines were attributed to the PREVIOUS file.
        if (line.startsWith(QLatin1String("diff --cc "))
            || line.startsWith(QLatin1String("diff --combined "))) {
            flush();
            continue;   // haveFile stays false until the next `diff --git`
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

QVector<AddedLine> addedLines(const DiffFile &file, const QString &match) {
    QVector<AddedLine> out;
    for (const DiffHunk &h : file.hunks) {
        int newLine = h.newStart;
        for (const QString &l : h.lines) {
            if (l.startsWith(QLatin1Char(' '))) {
                ++newLine;
            } else if (l.startsWith(QLatin1Char('+'))) {
                const QString text = l.mid(1);
                if (match.isEmpty() || text.contains(match))
                    out.append({newLine, text});
                ++newLine;
            }
            // `-` lines and "\ No newline" do not exist in the new file.
        }
    }
    return out;
}

QVector<LogRecord> parseLogNumstat(const QByteArray &out) {
    QVector<LogRecord> records;
    const QStringList raw =
        QString::fromUtf8(out).split(QChar(0x1e), Qt::SkipEmptyParts);
    for (const QString &rec : raw) {
        const int end = rec.indexOf(QChar(0x1d));
        if (end < 0) continue;   // a record cut by the stdout cap
        LogRecord r;
        r.fields = rec.left(end).split(QChar(0x1f));
        const QStringList lines =
            rec.mid(end + 1).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            // "<added>\t<removed>\t<path>"; a binary file shows "-\t-".
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 3) continue;
            NumstatRow row;
            bool ok = false;
            const int a = parts.at(0).toInt(&ok);
            if (ok) row.added = a;
            const int d = parts.at(1).toInt(&ok);
            if (ok) row.removed = d;
            row.path = parts.mid(2).join(QLatin1Char('\t'));
            r.files.append(row);
        }
        records.append(r);
    }
    return records;
}

QVector<GrepHit> parseGrepZ(const QByteArray &out, const QString &rev) {
    QVector<GrepHit> hits;
    const QString prefix = rev.isEmpty() ? QString() : rev + QLatin1Char(':');
    for (const QByteArray &lb : out.split('\n')) {
        // "<path>\0<line>\0<text>"; the text may hold NULs of its own.
        const int a = lb.indexOf('\0');
        const int b = a < 0 ? -1 : lb.indexOf('\0', a + 1);
        if (b < 0) continue;
        GrepHit h;
        h.path = QString::fromUtf8(lb.left(a));
        if (!prefix.isEmpty() && h.path.startsWith(prefix))
            h.path.remove(0, prefix.size());
        h.line = lb.mid(a + 1, b - a - 1).toInt();
        h.text = QString::fromUtf8(lb.mid(b + 1));
        hits.append(h);
    }
    return hits;
}

QString rangeTip(const QString &range) {
    const int dots = range.lastIndexOf(QLatin1String(".."));
    if (dots < 0) return range;
    const QString tip = range.mid(dots + 2);
    return tip.isEmpty() ? QStringLiteral("HEAD") : tip;
}

}  // namespace GitWrap
