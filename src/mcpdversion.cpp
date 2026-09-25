// ANTS-5340 — see mcpdversion.h.

#include "mcpdversion.h"

// cppcheck-suppress missingInclude  // ANTS-1682: generated at build time
#include "build_info.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

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

}  // namespace mcpd
