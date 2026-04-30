// ants-helper command-line entry point (ANTS-1116 v1).
//
// Usage:
//   ants-helper drift-check [request-json | -] [--repo-root <path>]
//   ants-helper list
//   ants-helper --help | -h
//
// Reads optional JSON request from argv (first non-flag positional
// after the subcommand) or from stdin if argv is `-` / absent.
// Writes one JSON object per invocation to stdout. Exit codes per
// `docs/specs/ANTS-1116.md` § INV-8: 0=success, 1=handler error,
// 2=usage error, 3=drift detected.

#include "antshelper.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QTextStream>

namespace {

void writeStderr(const QString &msg) {
    QTextStream(stderr) << msg << '\n';
}

void writeStdout(const QString &msg) {
    QTextStream(stdout) << msg << '\n';
}

QJsonObject parseRequest(const QString &raw, QString *errMsg) {
    if (raw.trimmed().isEmpty()) return {};
    QJsonParseError err;
    const QJsonDocument doc =
        QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        if (errMsg) *errMsg = err.errorString();
        return {};
    }
    // ANTS-1123 indie-review F6: a non-object payload (`[]`, `42`,
    // `"foo"`) used to silently degrade to an empty `{}`. That hid
    // shape errors from upstream callers — callers must learn that
    // the contract is "object or nothing." Surface as a usage error.
    if (!doc.isObject()) {
        if (errMsg) *errMsg = QStringLiteral(
            "request body must be a JSON object");
        return {};
    }
    return doc.object();
}

QString readStdin(bool *opened = nullptr) {
    QFile f;
    const bool ok = f.open(stdin, QIODevice::ReadOnly);
    if (opened) *opened = ok;
    if (!ok) return {};
    return QString::fromUtf8(f.readAll());
}

QJsonObject listSubcommands() {
    QJsonObject obj;
    QJsonArray arr;
    arr.append(QStringLiteral("drift-check"));
    obj.insert(QStringLiteral("ok"), true);
    obj.insert(QStringLiteral("subcommands"), arr);
    return obj;
}

void printHelp() {
    writeStdout(QStringLiteral(
        "ants-helper — local-subagent CLI for Claude Code "
        "(ANTS-1116 v1).\n\n"
        "Usage:\n"
        "  ants-helper drift-check [request-json] [--repo-root <path>]\n"
        "  ants-helper list\n"
        "  ants-helper --help | -h\n\n"
        "Subcommands:\n"
        "  drift-check   Run packaging/check-version-drift.sh, return "
        "structured\n"
        "                JSON. Exit 0 = clean, 3 = drift detected, "
        "1 = handler error.\n"
        "  list          Print the v1 subcommand list as JSON.\n"
        "\n"
        "Response shape:\n"
        "  { \"ok\": true,  \"data\": { ... } }   on success\n"
        "  { \"ok\": false, \"error\": \"...\", \"code\": \"...\" }   "
        "on handler error\n"
        "\n"
        "Request input is optional JSON; pass it on argv after the "
        "subcommand,\nor pipe it on stdin (or pass `-` as the argv "
        "to force stdin).\n"));
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();

    if (args.size() < 2 ||
        args[1] == QStringLiteral("-h") ||
        args[1] == QStringLiteral("--help")) {
        printHelp();
        return args.size() < 2 ? 2 : 0;
    }

    const QString cmd = args[1];

    QString repoRoot = QDir::currentPath();
    QString jsonArg;
    for (int i = 2; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--repo-root") && i + 1 < args.size()) {
            repoRoot = args[++i];
        } else if (jsonArg.isEmpty()) {
            jsonArg = args[i];
        }
    }
    if (jsonArg == QStringLiteral("-") || jsonArg.isEmpty()) {
        bool stdinOpened = true;
        jsonArg = readStdin(&stdinOpened);
        // ANTS-1123 indie-review F7: a stdin open failure used to
        // silently produce empty input, masquerading as "no request
        // body." Surface it on the explicit `-` path (where the
        // user opted into stdin) so a missing-stdin scenario doesn't
        // get confused with a default empty request.
        if (!stdinOpened && args.size() > 2 &&
            args[2] == QStringLiteral("-")) {
            QJsonObject err;
            err.insert(QStringLiteral("ok"), false);
            err.insert(QStringLiteral("error"),
                       QStringLiteral("stdin not readable"));
            err.insert(QStringLiteral("code"),
                       QStringLiteral("stdin_unreadable"));
            writeStdout(AntsHelper::jsonToCompactString(err));
            writeStderr(QStringLiteral("stdin not readable"));
            return 2;
        }
    }

    QString parseErr;
    const QJsonObject request = parseRequest(jsonArg, &parseErr);
    if (!parseErr.isEmpty()) {
        // Stay in the unified envelope shape on the stdout side so
        // Claude consumers parsing JSON don't have to special-case
        // usage errors. Stderr line stays for human users.
        // ANTS-1123 indie-review F1.
        QJsonObject err;
        err.insert(QStringLiteral("ok"), false);
        err.insert(QStringLiteral("error"),
                   QStringLiteral("invalid JSON: %1").arg(parseErr));
        err.insert(QStringLiteral("code"), QStringLiteral("usage_error"));
        writeStdout(AntsHelper::jsonToCompactString(err));
        writeStderr(QStringLiteral("invalid JSON: %1").arg(parseErr));
        return 2;
    }

    int exitCode = 0;
    QJsonObject response;
    if (cmd == QStringLiteral("drift-check")) {
        response = AntsHelper::driftCheck(request, repoRoot, &exitCode);
    } else if (cmd == QStringLiteral("list")) {
        response = listSubcommands();
        exitCode = 0;
    } else {
        // Same unified-envelope discipline on the unknown-subcommand
        // path. ANTS-1123 indie-review F1.
        QJsonObject err;
        err.insert(QStringLiteral("ok"), false);
        err.insert(QStringLiteral("error"),
                   QStringLiteral("unknown subcommand: %1").arg(cmd));
        err.insert(QStringLiteral("code"), QStringLiteral("usage_error"));
        writeStdout(AntsHelper::jsonToCompactString(err));
        writeStderr(QStringLiteral("unknown subcommand: %1").arg(cmd));
        return 2;
    }

    writeStdout(AntsHelper::jsonToCompactString(response));
    return exitCode;
}
