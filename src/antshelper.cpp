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

    // Reject obviously hostile --repo-root inputs. Threat model: Claude
    // Code (or any caller piping JSON over stdin) supplies the path —
    // not a co-UID local attacker. Refuse traversal substrings and
    // control bytes (NUL); canonicalise so the script lookup runs on
    // the resolved real path rather than whatever symlink chain the
    // caller supplied. Do NOT require a `.git` directory — INV-5 (bogus
    // repo root) doesn't restrict the helper to git checkouts; tests
    // pass tmpdirs with no VCS.
    if (repoRoot.contains(QStringLiteral("..")) || repoRoot.contains(QChar('\0'))) {
        setExit(1);
        return errorObj(
            QStringLiteral("repoRoot contains traversal or control bytes"),
            QStringLiteral("invalid_repo_root"));
    }
    const QString canonicalRoot = QFileInfo(repoRoot).canonicalFilePath();
    const QString resolvedRoot = canonicalRoot.isEmpty() ? repoRoot : canonicalRoot;

    const QString script = QDir(resolvedRoot).absoluteFilePath(
        QStringLiteral("packaging/check-version-drift.sh"));
    if (!QFileInfo::exists(script)) {
        setExit(1);
        return errorObj(
            QStringLiteral("drift script not found at %1").arg(script),
            QStringLiteral("missing_script"));
    }

    QProcess proc;
    proc.setWorkingDirectory(resolvedRoot);
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
    if (!proc.waitForFinished(60000)) {
        // Hung script: kill and surface the timeout. Without this branch
        // QProcess::exitStatus() returns NormalExit + exitCode() returns 0
        // for a still-running process — the worst possible failure mode
        // for a drift-detector (silent false-clean).
        proc.kill();
        proc.waitForFinished(1000);
        setExit(1);
        return errorObj(
            QStringLiteral("drift script timed out after 60s"),
            QStringLiteral("script_timeout"));
    }

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
