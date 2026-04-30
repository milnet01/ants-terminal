#include "antshelper.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>

namespace AntsHelper {

namespace {

QJsonObject errorObj(const QString &msg, const QString &code) {
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), msg);
    o.insert(QStringLiteral("code"), code);
    return o;
}

QJsonObject okObj(const QJsonObject &data) {
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    o.insert(QStringLiteral("data"), data);
    return o;
}

}  // namespace

QString jsonToCompactString(const QJsonObject &obj) {
    return QString::fromUtf8(
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QJsonObject driftCheck(const QJsonObject & /*request*/,
                       const QString &repoRoot,
                       int *exitCodeOut) {
    auto setExit = [&](int code) { if (exitCodeOut) *exitCodeOut = code; };

    if (!QFileInfo(repoRoot).isDir()) {
        setExit(1);
        return errorObj(
            QStringLiteral("repoRoot does not exist: %1").arg(repoRoot),
            QStringLiteral("missing_repo_root"));
    }

    const QString script = QDir(repoRoot).absoluteFilePath(
        QStringLiteral("packaging/check-version-drift.sh"));
    if (!QFileInfo::exists(script)) {
        setExit(1);
        return errorObj(
            QStringLiteral("drift script not found at %1").arg(script),
            QStringLiteral("missing_script"));
    }

    QProcess proc;
    proc.setWorkingDirectory(repoRoot);
    proc.setProgram(QStringLiteral("bash"));
    proc.setArguments({script});
    proc.start();
    if (!proc.waitForStarted(5000)) {
        setExit(1);
        // INV-5: error string is exactly "bash unavailable"
        // (spec contract; ANTS-1123 indie-review F2).
        return errorObj(
            QStringLiteral("bash unavailable"),
            QStringLiteral("missing_bash"));
    }
    proc.waitForFinished(60000);

    if (proc.exitStatus() != QProcess::NormalExit) {
        setExit(1);
        // QProcess::exitCode() is unspecified for CrashExit (Qt docs);
        // don't substitute it into the message. ANTS-1123 indie-review F3.
        return errorObj(
            QStringLiteral("drift script killed by signal"),
            QStringLiteral("script_signaled"));
    }

    const QString stdoutText =
        QString::fromUtf8(proc.readAllStandardOutput());
    const int exitCode = proc.exitCode();

    if (exitCode == 0) {
        QJsonObject data;
        data.insert(QStringLiteral("clean"), true);
        setExit(0);
        return okObj(data);
    }

    QJsonArray violations;
    static const QRegularExpression rxLine(
        QStringLiteral("^([^:]+):(\\d+):\\s*(.+)$"));
    // Bind split() to a local first — clazy `range-loop-detach`
    // (ANTS-1122 audit-fold-in 2026-04-30): iterating over the
    // temporary returned by split() detaches the QStringList.
    const QStringList lines = stdoutText.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const auto m = rxLine.match(line);
        if (m.hasMatch()) {
            QJsonObject v;
            v.insert(QStringLiteral("file"), m.captured(1));
            v.insert(QStringLiteral("line"), m.captured(2).toInt());
            v.insert(QStringLiteral("message"), m.captured(3).trimmed());
            violations.append(v);
        }
    }

    QJsonObject data;
    data.insert(QStringLiteral("clean"), false);
    data.insert(QStringLiteral("violations"), violations);
    data.insert(QStringLiteral("raw"), stdoutText);
    data.insert(QStringLiteral("exit_code"), exitCode);
    setExit(3);
    return okObj(data);
}

}  // namespace AntsHelper
