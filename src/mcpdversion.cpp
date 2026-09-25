// ANTS-5340 — see mcpdversion.h.

#include "mcpdversion.h"

// cppcheck-suppress missingInclude  // ANTS-1682: generated at build time
#include "build_info.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

#include <unistd.h>

#ifndef ANTS_VERSION
#define ANTS_VERSION "0.0.0"
#endif

namespace mcpd {

namespace {

bool isExecutableFile(const QString &path) {
    const QFileInfo fi(path);
    return !path.isEmpty() && fi.isFile() && fi.isExecutable();
}

// The user-level registration only: that is the one `claude mcp add` writes
// by default, and the one this project's README tells the user to run.
QString registeredCommand(const QString &claudeJsonPath) {
    QFile f(claudeJsonPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    return root.value(QStringLiteral("mcpServers")).toObject()
        .value(QStringLiteral("ants")).toObject()
        .value(QStringLiteral("command")).toString();
}

// Field 4 of /proc/<pid>/stat, read after the last ')' because the command
// name in field 2 may itself hold spaces or parentheses.
qint64 parentOf(qint64 pid) {
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QByteArray stat = f.readAll();
    const qsizetype close = stat.lastIndexOf(')');
    if (close < 0) return 0;
    const QList<QByteArray> fields = stat.mid(close + 2).split(' ');
    return fields.size() > 1 ? fields[1].toLongLong() : 0;
}

QList<qint64> ancestorsOf(qint64 pid) {
    QList<qint64> chain;
    for (qint64 p = parentOf(pid); p > 0 && !chain.contains(p); p = parentOf(p))
        chain.append(p);
    return chain;
}

}  // namespace

QString versionLine() {
    return QStringLiteral("ants-mcpd %1 · %2 %3 (%4) · commit %5")
        .arg(QStringLiteral(ANTS_VERSION),
             QString::fromLatin1(ANTS_BUILD_DATE),
             QString::fromLatin1(ANTS_BUILD_TIME),
             QString::fromLatin1(ANTS_BUILD_TYPE),
             QString::fromLatin1(ANTS_BUILD_COMMIT));
}

QString locateBinary(const QString &claudeJsonPath, const QString &appDir) {
    QString registered = registeredCommand(claudeJsonPath);
    if (isExecutableFile(registered)) return registered;
    QString sibling = appDir + QStringLiteral("/ants-mcpd");
    if (isExecutableFile(sibling)) return sibling;
    return QStandardPaths::findExecutable(QStringLiteral("ants-mcpd"));
}

QString queryVersion(const QString &binary, int timeoutMs) {
    if (binary.isEmpty()) return {};
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(binary, {QStringLiteral("--version")});
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return {};
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    return out.section(QLatin1Char('\n'), 0, 0).trimmed();
}

QList<RunningCopy> runningCopies(int timeoutMs) {
    // readlink on another user's /proc/<pid>/exe fails, so only this user's
    // processes are ever read.
    static const QString kDeleted = QStringLiteral(" (deleted)");
    QList<RunningCopy> out;
    const QDir proc(QStringLiteral("/proc"));
    for (const QString &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const qint64 pid = entry.toLongLong(&ok);
        if (!ok || pid <= 0) continue;
        const QString exePath = QStringLiteral("/proc/%1/exe").arg(pid);
        // Raw readlink: the kernel's " (deleted)" suffix must survive intact.
        char buf[4096];
        const ssize_t n = ::readlink(QFile::encodeName(exePath).constData(),
                                     buf, sizeof(buf));
        if (n <= 0) continue;
        QString target = QFile::decodeName(QByteArray(buf, n));
        RunningCopy copy;
        copy.pid = pid;
        if (target.endsWith(kDeleted)) {
            copy.replaced = true;
            target.chop(kDeleted.size());
        }
        // A rebuild's linker may leave a temporary name (".ants-mcpd.NNN")
        // behind the deleted target, so match that too.
        const QString base = target.section(QLatin1Char('/'), -1);
        if (base != QLatin1String("ants-mcpd")
            && !base.startsWith(QLatin1String(".ants-mcpd.")))
            continue;
        copy.version = queryVersion(exePath, timeoutMs);
        copy.ancestors = ancestorsOf(pid);
        out.append(copy);
    }
    return out;
}

bool isStale(const RunningCopy &copy, const QString &diskVersion) {
    return copy.replaced || copy.version.isEmpty() || copy.version != diskVersion;
}

}  // namespace mcpd
