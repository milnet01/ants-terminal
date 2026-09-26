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

#include "claudeintegration.h"
#include "config.h"
#include "debuglog.h"
#include "mcpdforwarder.h"
#include "mcpdversion.h"
#include "mcpprojection.h"
#include "mcpspill.h"
#include "mcptoolregistry.h"
#include "remotecontrol.h"
#include "rootprovider.h"
#include "secureio.h"
#include "tokenusageengine.h"
#include "verifytrust.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

void writeStdout(const QByteArray &line) {
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char **argv) {
    // ANTS-5340 — before anything reads stdin or the config: Help → About
    // asks this on every open.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            writeStdout(mcpd::versionLine().toUtf8() + '\n');
            return 0;
        }
    }

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

    ants::ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    // ANTS-1337 — verify_changes is served here, so it needs the trust gate
    // the terminal wires too. Without a client, VerifyEngine honours a repo's
    // .ants/verify.json unconditionally. There is no window to prompt from,
    // so the file-backed client answers Headless for any SHA not already in
    // verify-trust.json, and the engine falls back to auto-detect.
    rc.setVerifyTrustClient(std::make_unique<VerifyTrust::FilePersistedTrustClient>());
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

    // ANTS-5311 § 2.4 — this process's usage snapshot, so the terminal's
    // token_usage and savings chip count the calls served here. The lock is
    // held until exit and marks the snapshot live; O_CLOEXEC keeps `rg` and
    // `git` children from inheriting it and outliving us with it.
    const QString usageDir = TokenUsageEngine::peerSnapshotDir();
    const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
    const QString usageStem = QStringLiteral("%1-%2")
        .arg(QCoreApplication::applicationPid()).arg(startedMs);
    int usageLock = -1;
    // ensurePrivateDir creates it 0700 from the start; mkpath + chmod left a
    // window at the umask's mode in which other users could list it.
    if (ensurePrivateDir(usageDir)) {
        const QByteArray lockPath =
            QFile::encodeName(usageDir + QLatin1Char('/') + usageStem + QStringLiteral(".lock"));
        usageLock = ::open(lockPath.constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (usageLock >= 0 && ::flock(usageLock, LOCK_EX) != 0) {
            ::close(usageLock);
            usageLock = -1;
        }
    }
    if (usageLock < 0)
        std::fprintf(stderr, "ants-mcpd: usage snapshots disabled (cannot lock in %s)\n",
                     qPrintable(usageDir));

    // Sessions ended by a second `initialize` in this process, so a reset of
    // the pipeline's tracker loses nothing. At most kMaxTokenProjects roots:
    // a new root displaces the one with the fewest bytes.
    QHash<QString, TokenUsageEngine::ToolCounter> accTools;
    QHash<QString, qint64> accBytes;
    const auto addBytes = [](QHash<QString, qint64> &into, const QHash<QString, qint64> &from) {
        for (auto it = from.cbegin(); it != from.cend(); ++it) {
            if (!into.contains(it.key()) && into.size() >= TokenUsageEngine::kMaxPeerProjects) {
                auto smallest = std::min_element(into.begin(), into.end());
                if (*smallest >= it.value()) continue;
                into.erase(smallest);
            }
            into[it.key()] += it.value();
        }
    };
    const auto writeUsage = [&] {
        if (usageLock < 0) return;
        QHash<QString, TokenUsageEngine::ToolCounter> tools = accTools;
        TokenUsageEngine::addCounters(tools, pipeline.tokenUsageCounters());
        QHash<QString, qint64> bytes = accBytes;
        addBytes(bytes, pipeline.sessionSavedBytesByProject());
        QSaveFile f(usageDir + QLatin1Char('/') + usageStem + QStringLiteral(".json"));
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QJsonDocument(TokenUsageEngine::snapshotToJson(
                    tools, bytes, QCoreApplication::applicationPid(), startedMs,
                    QDateTime::currentMSecsSinceEpoch())).toJson(QJsonDocument::Compact));
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.commit();
    };
    writeUsage();   // at once, so a lock with no json lasts only a moment

    QTimer usageFlush;
    usageFlush.setSingleShot(true);
    usageFlush.setInterval(2000);
    QObject::connect(&usageFlush, &QTimer::timeout, &app, writeUsage);
    // Started, never restarted: a busy helper still writes every 2 s.
    QObject::connect(&pipeline, &ClaudeIntegration::tokensSavedUpdated, &app,
                     [&usageFlush] { if (!usageFlush.isActive()) usageFlush.start(); });
    QObject::connect(&pipeline, &ClaudeIntegration::tokenSessionEnding, &app, [&] {
        TokenUsageEngine::addCounters(accTools, pipeline.tokenUsageCounters());
        addBytes(accBytes, pipeline.sessionSavedBytesByProject());
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, writeUsage);

    // SIGTERM / SIGINT reach aboutToQuit through a self-pipe, so a client that
    // stops the server by signal still gets the final write.
    static int sigPipe[2] = {-1, -1};
    std::unique_ptr<QSocketNotifier> sigNotifier;
    if (::pipe2(sigPipe, O_CLOEXEC | O_NONBLOCK) == 0) {
        struct sigaction sa {};
        sa.sa_handler = [](int) {
            const char c = 1;
            [[maybe_unused]] const ssize_t n = ::write(sigPipe[1], &c, 1);
        };
        sigemptyset(&sa.sa_mask);
        ::sigaction(SIGTERM, &sa, nullptr);
        ::sigaction(SIGINT, &sa, nullptr);
        sigNotifier = std::make_unique<QSocketNotifier>(sigPipe[0], QSocketNotifier::Read);
        QObject::connect(sigNotifier.get(), &QSocketNotifier::activated, &app,
                         [] { QCoreApplication::quit(); });
    }

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
        if (pending.size() > qsizetype{256} * 1024) pending.clear();
    });

    return QCoreApplication::exec();
}
