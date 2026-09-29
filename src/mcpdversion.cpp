// ANTS-5340 — see mcpdversion.h.

#include "mcpdversion.h"

// cppcheck-suppress missingInclude  // ANTS-1682: generated at build time
#include "build_info.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
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
    return queryVersion(Launch{binary, {}}, timeoutMs);
}

// ANTS-5558 — the args go before `--version`, so an AppImage registration is
// asked `$APPIMAGE --mcpd --version` rather than starting the terminal.
QString queryVersion(const Launch &launch, int timeoutMs) {
    if (launch.program.isEmpty()) return {};
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(launch.program, launch.args + QStringList{QStringLiteral("--version")});
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

Launch registeredLaunch(const QString &claudeJsonPath) {
    QFile f(claudeJsonPath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject ants = QJsonDocument::fromJson(f.readAll()).object()
        .value(QStringLiteral("mcpServers")).toObject()
        .value(QStringLiteral("ants")).toObject();
    Launch l;
    l.program = ants.value(QStringLiteral("command")).toString();
    for (const auto &a : ants.value(QStringLiteral("args")).toArray())
        l.args.append(a.toString());
    return l;
}

Launch locateLaunch(const QString &claudeJsonPath, const QString &appDir) {
    const Launch reg = registeredLaunch(claudeJsonPath);
    if (isExecutableFile(reg.program)) return reg;
    // A fallback binary is not what was registered, so it runs with no args.
    return Launch{locateBinary(QString(), appDir), {}};
}

int projectRegistrationCount(const QString &claudeJsonPath) {
    QFile f(claudeJsonPath);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QJsonObject projects = QJsonDocument::fromJson(f.readAll()).object()
        .value(QStringLiteral("projects")).toObject();
    int n = 0;
    for (auto it = projects.constBegin(); it != projects.constEnd(); ++it)
        if (it.value().toObject().value(QStringLiteral("mcpServers"))
                .toObject().contains(QStringLiteral("ants")))
            ++n;
    return n;
}

}  // namespace mcpd
