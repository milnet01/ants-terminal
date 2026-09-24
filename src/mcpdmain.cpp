// ANTS-4932 — ants-mcpd: the project-scoped Ants MCP verbs, served over stdio.
//
// Claude Code launches this in place of tools/mcp-bridge.py. It answers every
// verb that reads no tab or terminal state itself, and forwards the rest to a
// running terminal. Because the client starts it, a rebuilt ants-mcpd is
// picked up by a client reconnect: no terminal relaunch, so no Claude Code
// session in any tab is killed (docs/specs/ANTS-4932-standalone-mcp-server.md).
//
// stdout carries the protocol and nothing else: one JSON-RPC reply per line.
// Diagnostics go to stderr.

#include "build_info.h"
#include "claudeintegration.h"
#include "config.h"
#include "debuglog.h"
#include "mcpdforwarder.h"
#include "mcpprojection.h"
#include "mcpspill.h"
#include "mcptoolregistry.h"
#include "remotecontrol.h"
#include "remotecontrol_internal.h"
#include "rootprovider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QSocketNotifier>

#include <cstdio>
#include <unistd.h>

namespace {

// § 2.4 — with no tabs, the fallback root is this process's own cwd, which
// the client set to its working directory when it launched us.
class ServerCwdRootProvider final : public ants::RootProvider {
public:
    QString fallbackRoot() const override { return QDir::currentPath(); }
    QString fallbackRoadmapPath() const override {
        const QString root = QFileInfo(fallbackRoot()).canonicalFilePath();
        return root.isEmpty() ? QString() : rcdetail::findRoadmapUnder(root);
    }
    std::optional<int> fallbackTab() const override { return std::nullopt; }
    ants::ResolvedRoot::Source fallbackSource() const override {
        return ants::ResolvedRoot::Source::ServerCwd;
    }
    std::optional<int> tabForCwd(const QString &) const override {
        return std::nullopt;
    }
};

void writeStdout(const QByteArray &line) {
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // Same name as the terminal, so every AppConfigLocation path agrees.
    app.setApplicationName(QStringLiteral("Ants Terminal"));
    app.setApplicationVersion(QStringLiteral(ANTS_VERSION));

    const QByteArray debugSpec = qgetenv("ANTS_DEBUG");
    if (!debugSpec.isEmpty())
        DebugLog::setActive(DebugLog::parseCategories(
            QString::fromLocal8Bit(debugSpec)));

    // § 2.3 — the same config the terminal reads, including the master gate.
    // Read once: a Settings change reaches this process on the next client
    // reconnect, which starts a fresh one.
    const Config config;
    mcp::setTerseDefault(config.claudeMcpTerseResponses());
    mcp::setHintLatchEnabled(config.claudeMcpHintLatch());
    mcp::setOffloadConfig(config.claudeMcpOffloadLargeResults(),
                          config.claudeMcpOffloadThresholdBytes(),
                          config.claudeMcpOffloadHeadBytes());
    const mcp::ProjectQueryConfig projectQuery{
        config.claudeMcpProjectQueryEnabled(),
        config.claudeMcpProjectQueryTimeoutMs(),
        config.claudeMcpProjectQueryResultCapBytes()};

    ClaudeIntegration pipeline;
    pipeline.setMcpEnabled(config.claudeMcpEnabled());
    pipeline.setServerName(QStringLiteral("ants-mcpd"));

    ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    rc.setMcpVerbVocabularyProvider(
        [&pipeline] { return pipeline.registeredToolNames(); });

    mcp::RegistryHost host;
    host.ci    = &pipeline;
    host.roots = &roots;
    host.projectQueryConfig = [projectQuery]() -> std::optional<mcp::ProjectQueryConfig> {
        return projectQuery;
    };
    mcp::registerProjectScopedVerbs(pipeline, [&rc] { return &rc; }, host);

    // § 2.5 — every terminal-scoped verb goes to the terminal, whole.
    mcpd::Forwarder forwarder;
    QSet<QString> forwarded;
    for (const QString &name : mcp::terminalScopedVerbNames()) forwarded.insert(name);
    pipeline.setForwarder(forwarded,
        [&forwarder](const QByteArray &line, ClaudeIntegration::ForwardReplyFn onReply,
                     ClaudeIntegration::ForwardFailFn onFail) {
            forwarder.forward(line, std::move(onReply), std::move(onFail));
        });

    // One channel for the whole session: replies may arrive out of order, and
    // JSON-RPC matches them by id.
    McpReplyChannel out(
        [](const QByteArray &line) { writeStdout(line); },
        [] {}, [] { return true; });

    QByteArray pending;
    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&stdinNotifier, &QSocketNotifier::activated, &app,
                     [&](QSocketDescriptor, QSocketNotifier::Type) {
        char chunk[65536];
        const ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n <= 0) {
            // EOF or error: the client has gone, so nobody is left to answer.
            stdinNotifier.setEnabled(false);
            QCoreApplication::quit();
            return;
        }
        pending.append(chunk, n);
        qsizetype nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(nl).trimmed();
            pending.remove(0, nl + 1);
            if (!line.isEmpty()) pipeline.handleMcpLine(line, &out);
        }
        // ANTS-1659 — the same request ceiling the socket enforces.
        if (pending.size() > 256 * 1024) pending.clear();
    });

    return QCoreApplication::exec();
}
