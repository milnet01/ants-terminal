// ANTS-5506 — see mcpdcall.h and docs/specs/ANTS-5506-mcpd-call.md.

#include "mcpdcall.h"

#include "claudeintegration.h"
#include "mcptoolregistry.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>

namespace mcpd {

namespace {

constexpr QLatin1String kWrapOpen("<ants_mcp_data");
constexpr QLatin1String kWrapClose("</ants_mcp_data>");

// § 2.1 — change what a reply looks like, never what is checked.
const QStringList &replyShapingKeys() {
    static const QStringList keys{QStringLiteral("compact"), QStringLiteral("offload"),
                                  QStringLiteral("fields"), QStringLiteral("raw")};
    return keys;
}

// A JSON argument that must be an object. Empty text is `{}`.
bool parseObject(const QByteArray &text, QJsonObject *out) {
    if (text.trimmed().isEmpty()) {
        *out = {};
        return true;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    *out = doc.object();
    return true;
}

void writeStdout(const QByteArray &bytes) {
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}

int usage(const QString &message) {
    std::fprintf(stderr, "ants-mcpd --call: %s\n", qPrintable(message));
    return CallUsage;
}

// One JSON-RPC request through the pipeline; its reply line, or empty when the
// request ended with no reply.
QByteArray dispatch(ClaudeIntegration &pipeline, int id, const QString &method,
                    const QJsonObject &params) {
    QJsonObject rpc{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("method"), method}};
    if (!params.isEmpty()) rpc.insert(QStringLiteral("params"), params);
    QByteArray replyLine;
    bool done = false;
    QEventLoop loop;
    McpReplyChannel channel(
        [&](const QByteArray &line) { replyLine = line; done = true; loop.quit(); },
        [&] { done = true; loop.quit(); }, [] { return true; });
    pipeline.handleMcpLine(QJsonDocument(rpc).toJson(QJsonDocument::Compact), &channel);
    if (!done) loop.exec();
    return replyLine;
}

} // namespace

CallRequest parseCallRequest(const QStringList &args) {
    CallRequest req;
    QStringList positional;
    for (const QString &a : args) {
        if (a == QLatin1String("--call")) {
            req.isCall = true;
        } else if (a == QLatin1String("--exit-code")) {
            req.exitCode = true;
        } else if (a.startsWith(QLatin1String("--"))) {
            req.usageError = QStringLiteral("unknown flag %1").arg(a);
        } else {
            positional << a;
        }
    }
    if (!req.isCall || !req.usageError.isEmpty()) return req;
    if (positional.isEmpty()) {
        req.usageError = QStringLiteral("no verb given");
        return req;
    }
    if (positional.size() > 2) {
        req.usageError = QStringLiteral("too many arguments");
        return req;
    }
    req.verb = positional.at(0);
    if (positional.size() == 2) {
        if (positional.at(1) == QLatin1String("-")) {
            req.readStdin = true;
        } else if (!parseObject(positional.at(1).toUtf8(), &req.args)) {
            req.usageError = QStringLiteral("the JSON argument is not an object");
        }
    }
    return req;
}

QJsonObject prepareCallArguments(QJsonObject args, const QString &processCwd) {
    for (const QString &key : replyShapingKeys()) args.remove(key);
    if (!args.contains(QStringLiteral("caller_cwd")))
        args.insert(QStringLiteral("caller_cwd"), processCwd);
    return args;
}

QString unwrapToolText(const QString &text) {
    const QString trimmed = text.trimmed();
    if (!trimmed.startsWith(kWrapOpen) || !trimmed.endsWith(kWrapClose)) return text;
    const qsizetype bodyStart = trimmed.indexOf(QLatin1Char('>')) + 1;
    const qsizetype bodyEnd = trimmed.size() - kWrapClose.size();
    if (bodyStart <= 0 || bodyEnd < bodyStart) return text;
    return trimmed.mid(bodyStart, bodyEnd - bodyStart);
}

CallExit exitCodeFor(const QJsonObject &envelope, bool envelopeParsed,
                     bool exitCodeFlag) {
    // § 2.2's order; usage was settled before dispatch.
    if (!envelopeParsed) return CallFailed;
    if (!envelope.value(QStringLiteral("ok")).toBool()) return CallFailed;
    if (!exitCodeFlag) return CallClean;
    if (!envelope.value(QStringLiteral("check_errors")).toArray().isEmpty())
        return CallFailed;
    if (!envelope.contains(QStringLiteral("findings"))) return CallUsage;
    if (!envelope.value(QStringLiteral("findings")).toArray().isEmpty())
        return CallFindings;
    return CallClean;
}

int runCall(const CallRequest &request, ClaudeIntegration &pipeline) {
    if (!request.usageError.isEmpty()) return usage(request.usageError);
    if (mcp::terminalScopedVerbNames().contains(request.verb))
        return usage(QStringLiteral("%1 needs a running terminal").arg(request.verb));
    if (!pipeline.registeredToolNames().contains(request.verb))
        return usage(QStringLiteral("no verb named %1").arg(request.verb));

    QJsonObject args = request.args;
    if (request.readStdin) {
        QFile in;
        if (!in.open(stdin, QIODevice::ReadOnly) || !parseObject(in.readAll(), &args))
            return usage(QStringLiteral("stdin is not a JSON object"));
    }
    args = prepareCallArguments(args, QDir::currentPath());

    // A client asks for tools/list before calling; tool_info refuses
    // `tools_not_ready` until something has.
    dispatch(pipeline, 1, QStringLiteral("tools/list"), {});
    const QByteArray replyLine = dispatch(
        pipeline, 2, QStringLiteral("tools/call"),
        QJsonObject{{QStringLiteral("name"), request.verb},
                    {QStringLiteral("arguments"), args}});

    const QJsonObject reply = QJsonDocument::fromJson(replyLine).object();
    const QString text = reply.value(QStringLiteral("result")).toObject()
                             .value(QStringLiteral("content")).toArray()
                             .at(0).toObject().value(QStringLiteral("text")).toString();
    if (text.isEmpty()) {
        std::fprintf(stderr, "ants-mcpd --call: no result: %s\n", replyLine.constData());
        return CallFailed;
    }
    const QString body = unwrapToolText(text);
    writeStdout(body.toUtf8() + '\n');

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body.toUtf8(), &err);
    const bool parsed = err.error == QJsonParseError::NoError && doc.isObject();
    return exitCodeFor(parsed ? doc.object() : QJsonObject{}, parsed, request.exitCode);
}

} // namespace mcpd
