// ANTS-4932 — ants-mcpd, the standalone MCP server. Contract: spec.md here,
// which cites docs/specs/ANTS-4932-standalone-mcp-server.md § 3.
//
// Every case but INV-2 runs the built ants-mcpd as a child process, because
// what is under test is that binary: its link closure, its stdio protocol, its
// root fallback, its forwarding and its writes to the shared store.

#include "mcpd_session.h"
#include "stub_terminal.h"

#include "claudeintegration.h"
#include "mcpdsocket.h"
#include "mcptoolregistry.h"
#include "remotecontrol.h"
#include "roadmapmigrate.h"
#include "roadmapmigrateload.h"
#include "roadmapstore.h"
#include "rootprovider.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocalServer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <atomic>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#ifndef ANTS_TERMINAL_BIN
#error "ANTS_TERMINAL_BIN compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

using ants_test::McpdSession;

namespace {

// A socket path with no listener: the "no terminal running" case.
QString deadSocket(const QTemporaryDir &tmp) {
    return tmp.filePath(QStringLiteral("no-terminal.sock"));
}

// readelf ships in binutils, which the compiler toolchain every suite-running
// carrier installs already depends on, so it is not in ci_workflow_deps'
// REQUIRED set. An empty result fails the case rather than skipping it.
QString readelfNeeded(const QString &binary) {
    QProcess p;
    p.start(QStringLiteral("readelf"), {QStringLiteral("-d"), binary});
    if (!p.waitForFinished(20000)) return {};
    return QString::fromUtf8(p.readAllStandardOutput());
}

class RecordingSink : public mcp::ToolSink {
public:
    QStringList names;
    void registerToolProvider(const QString &n, mcp::CallerCwdContract,
                              mcp::ToolHandler) override { names << n; }
    void registerToolProvider(const QString &n, mcp::CallerCwdContract,
                              mcp::RcHandler) override { names << n; }
    void registerToolProvider(const QString &n, mcp::CallerCwdContract,
                              mcp::DeferredToolHandler) override { names << n; }
};

QSet<QString> terminalScoped() {
    QSet<QString> s;
    for (const QString &n : mcp::terminalScopedVerbNames()) s.insert(n);
    return s;
}

// A migrated fixture project in the sandbox store, with a `work` section.
QString seedMigratedProject(const QTemporaryDir &tmp) {
    QByteArray md =
        "<!-- ants-roadmap-format: 1 -->\n\n# Demo \xE2\x80\x94 Roadmap\n\n";
    // The roadmap_log write paths refuse a file under 1 KiB.
    for (int i = 0; i < 20; ++i)
        md += "Padding line so the fixture clears the minimum parseable size.\n";
    md += "\n## Work\n\n"
          "- \xF0\x9F\x93\x8B [DEMO-0001] **An open item.**\n"
          "  Layman: A thing.\n  Kind: implement.\n  Source: seed.\n\n";
    const QString raw = tmp.filePath(QStringLiteral("proj"));
    QDir().mkpath(raw);
    QFile f(raw + QStringLiteral("/ROADMAP.md"));
    if (!f.open(QIODevice::WriteOnly) || f.write(md) != md.size()) return {};
    f.close();
    const QString root = QFileInfo(raw).canonicalFilePath();

    RoadmapStore store(RoadmapStore::defaultPath(),
                       RoadmapStore::kDefaultHistoryCapBytes,
                       RoadmapStore::Access::Bulk);
    QString err;
    if (!store.open(&err)) { ADD_FAILURE() << err.toStdString(); return {}; }
    const auto disc = RoadmapMigrate::findRoadmaps(root, &err);
    if (!disc) { ADD_FAILURE() << err.toStdString(); return {}; }
    const auto plan = RoadmapMigrate::planFrom(*disc, QStringLiteral("Demo"),
                                               QStringLiteral("demo-4932"));
    RoadmapMigrateLoad::Options opts;
    opts.changedAt   = QStringLiteral("2026-09-24T10:00:00Z");
    opts.projectRoot = root;
    const auto out = RoadmapMigrateLoad::load(store, plan, opts);
    if (!out.ok) { ADD_FAILURE() << out.error.toStdString(); return {}; }
    return root;
}

QJsonObject appendArgs(const QString &root, const QString &headline) {
    return QJsonObject{{"caller_cwd", root},      {"op", "append"},
                       {"section", "work"},        {"status", "planned"},
                       {"headline", headline},     {"kind", "implement"},
                       {"source", "test"},         {"layman", "A new thing."}};
}

}  // namespace

// INV-1 — no GUI library in ants-mcpd's link closure; ants-terminal is the
// positive control that the probe can see one.
TEST(StandaloneMcpServer, Inv1LinksNoGuiLibrary) {
    const QString mcpd = readelfNeeded(QStringLiteral(ANTS_MCPD_BIN));
    const QString term = readelfNeeded(QStringLiteral(ANTS_TERMINAL_BIN));
    ASSERT_TRUE(mcpd.contains(QStringLiteral("libQt6Core"))) << "readelf saw nothing";
    for (const char *lib : {"libQt6Gui", "libQt6Widgets", "libQt6DBus"}) {
        EXPECT_FALSE(mcpd.contains(QLatin1String(lib))) << "ants-mcpd needs " << lib;
        EXPECT_TRUE(term.contains(QLatin1String(lib)))
            << "positive control: ants-terminal should need " << lib;
    }
}

// INV-2 — the registry registers no terminal-scoped verb, and mainwindow.cpp
// registers nothing else.
TEST(StandaloneMcpServer, Inv2OneEnumerationOfTheProjectScopedSet) {
    RecordingSink sink;
    mcp::registerProjectScopedVerbs(sink, [] { return nullptr; });
    ASSERT_FALSE(sink.names.isEmpty());
    const QSet<QString> terminal = terminalScoped();
    for (const QString &n : sink.names)
        EXPECT_FALSE(terminal.contains(n)) << n.toStdString()
            << " is registered by the shared list but is terminal-scoped";

    QFile mw(QStringLiteral(ANTS_SOURCE_DIR "/src/mainwindow.cpp"));
    ASSERT_TRUE(mw.open(QIODevice::ReadOnly));
    const QString src = QString::fromUtf8(mw.readAll());
    static const QRegularExpression rx(QStringLiteral(R"RX(registerToolProvider\(\s*"([^"]+)")RX"));
    int seen = 0;
    for (auto it = rx.globalMatch(src); it.hasNext(); ++seen) {
        const QString n = it.next().captured(1);
        EXPECT_TRUE(terminal.contains(n)) << n.toStdString()
            << " is registered in mainwindow.cpp but is not terminal-scoped";
    }
    EXPECT_GT(seen, 0) << "no registration found in mainwindow.cpp";
}

// INV-3 — both hosts answer tools/list alike: the same names, and the same
// input schema for each.
TEST(StandaloneMcpServer, Inv3ToolsListAgreesWithTheTerminalPipeline) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    McpdSession mcpd(QStringLiteral(ANTS_SOURCE_DIR), deadSocket(tmp));
    ASSERT_TRUE(mcpd.started());
    const QJsonArray remote = mcpd.await(mcpd.send(QStringLiteral("tools/list")))
                                  .value(QStringLiteral("result")).toObject()
                                  .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(remote.isEmpty()) << mcpd.stderrText().toStdString();

    ClaudeIntegration ci;
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", &out);
    const QJsonArray local = QJsonDocument::fromJson(line).object()
                                 .value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    ASSERT_FALSE(local.isEmpty());

    QHash<QString, QJsonObject> localByName;
    for (const QJsonValue &v : local)
        localByName.insert(v.toObject().value(QStringLiteral("name")).toString(),
                           v.toObject());
    EXPECT_EQ(remote.size(), local.size());
    for (const QJsonValue &v : remote) {
        const QJsonObject t = v.toObject();
        const QString name = t.value(QStringLiteral("name")).toString();
        ASSERT_TRUE(localByName.contains(name)) << name.toStdString()
            << " is listed by ants-mcpd only";
        EXPECT_EQ(t.value(QStringLiteral("inputSchema")),
                  localByName.value(name).value(QStringLiteral("inputSchema")))
            << name.toStdString() << ": the input schemas differ";
    }
}

// INV-5 — with no terminal, a forwarded verb refuses no_terminal and a
// project-scoped verb still answers.
TEST(StandaloneMcpServer, Inv5NoTerminalRefusesForwardedVerbsOnly) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    McpdSession mcpd(QStringLiteral(ANTS_SOURCE_DIR), deadSocket(tmp));
    ASSERT_TRUE(mcpd.started());
    const QJsonObject tabs = mcpd.call(QStringLiteral("tab_list"), {});
    EXPECT_EQ(tabs.value(QStringLiteral("code")).toString(), QStringLiteral("no_terminal"))
        << QJsonDocument(tabs).toJson().toStdString();
    const QJsonObject lint = mcpd.call(QStringLiteral("spec_lint"),
        QJsonObject{{"caller_cwd", ANTS_SOURCE_DIR},
                    {"path", "docs/specs/ANTS-4932-standalone-mcp-server.md"}});
    EXPECT_TRUE(lint.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(lint).toJson().toStdString();
}

// INV-6 — no caller_cwd resolves to the server's own cwd, reported ServerCwd.
TEST(StandaloneMcpServer, Inv6FallbackIsTheServerCwd) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString fixture = tmp.filePath(QStringLiteral("fixture"));
    ASSERT_TRUE(QDir().mkpath(fixture));
    McpdSession mcpd(fixture, deadSocket(tmp));
    ASSERT_TRUE(mcpd.started());
    const QJsonObject info = mcpd.call(QStringLiteral("caller_cwd_info"), {});
    EXPECT_EQ(info.value(QStringLiteral("source")).toString(), QStringLiteral("ServerCwd"));
    EXPECT_EQ(info.value(QStringLiteral("resolved_cwd")).toString(),
              QFileInfo(fixture).canonicalFilePath());
}

// INV-8 — the forwarded request keeps caller_cwd byte-identical, and adds none.
TEST(StandaloneMcpServer, Inv8ForwardNeverSynthesisesCallerCwd) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants_test::StubTerminal stub(tmp.filePath(QStringLiteral("stub.sock")));
    ASSERT_TRUE(stub.listening());
    McpdSession mcpd(QStringLiteral(ANTS_SOURCE_DIR), stub.path());
    ASSERT_TRUE(mcpd.started());

    // A tab index and no caller_cwd: the gate lets it through, and the
    // forwarded arguments must still have no caller_cwd.
    const QJsonObject r1 = mcpd.call(QStringLiteral("get_text"), QJsonObject{{"tab", 0}});
    ASSERT_EQ(stub.requests().size(), 1) << QJsonDocument(r1).toJson().toStdString();
    const QJsonObject a1 = stub.requests().at(0).value(QStringLiteral("params")).toObject()
                               .value(QStringLiteral("arguments")).toObject();
    EXPECT_FALSE(a1.contains(QStringLiteral("caller_cwd")));
    EXPECT_EQ(a1.value(QStringLiteral("tab")).toInt(-1), 0);
    EXPECT_TRUE(r1.value(QStringLiteral("stub")).toBool()) << "reply not relayed";

    const QString cwd = QStringLiteral("/some/where/else");
    mcpd.call(QStringLiteral("get_text"), QJsonObject{{"caller_cwd", cwd}});
    ASSERT_EQ(stub.requests().size(), 2);
    EXPECT_EQ(stub.requests().at(1).value(QStringLiteral("params")).toObject()
                  .value(QStringLiteral("arguments")).toObject()
                  .value(QStringLiteral("caller_cwd")).toString(), cwd);
}

// INV-9 — writes from both hosts to one project: nothing lost, nothing broken.
TEST(StandaloneMcpServer, Inv9ConcurrentWritesFromBothHosts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedMigratedProject(tmp);
    ASSERT_FALSE(root.isEmpty());

    constexpr int kPerHost = 8;
    McpdSession mcpd(root, deadSocket(tmp));
    ASSERT_TRUE(mcpd.started());
    QList<int> ids;
    for (int i = 0; i < kPerHost; ++i)
        ids << mcpd.sendCall(QStringLiteral("roadmap_log"),
                             appendArgs(root, QStringLiteral("From ants-mcpd %1.").arg(i)));

    // The other host: this process, as the terminal runs it — a RemoteControl
    // driven from a worker thread, concurrently with the child.
    std::atomic<int> localOk{0};
    std::thread local([&] {
        ants::ServerCwdRootProvider roots;
        RemoteControl rc(nullptr, nullptr, &roots);
        for (int i = 0; i < kPerHost; ++i) {
            const QJsonObject r = rc.cmdRoadmapLog(
                appendArgs(root, QStringLiteral("From the terminal %1.").arg(i))).object();
            if (r.value(QStringLiteral("ok")).toBool()) ++localOk;
        }
    });
    int remoteOk = 0;
    for (int id : ids)
        if (McpdSession::payload(mcpd.await(id)).value(QStringLiteral("ok")).toBool())
            ++remoteOk;
    local.join();

    EXPECT_GE(remoteOk, 1) << "no append succeeded through ants-mcpd";
    EXPECT_GE(localOk.load(), 1) << "no append succeeded in-process";

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("inv9"));
        db.setDatabaseName(RoadmapStore::defaultPath());
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        ASSERT_TRUE(q.exec(QStringLiteral("PRAGMA integrity_check")) && q.next());
        EXPECT_EQ(q.value(0).toString(), QStringLiteral("ok"));
        ASSERT_TRUE(q.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM item i JOIN project p ON p.project_id = i.project_id "
            "WHERE p.export_slug = 'demo-4932'")));
        ASSERT_TRUE(q.exec() && q.next());
        // The seeded item plus one per successful append.
        EXPECT_EQ(q.value(0).toInt(), 1 + remoteOk + localOk.load());
        q.finish();
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("inv9"));
}

// INV-11 — both uid checks refuse a uid other than the expected one.
TEST(StandaloneMcpServer, Inv11UidChecksRefuseAnotherUid) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QLocalServer server;
    const QString path = tmp.filePath(QStringLiteral("own.sock"));
    ASSERT_TRUE(server.listen(path));
    const uid_t me = ::getuid();
    EXPECT_TRUE(mcpd::socketOwnedBy(path, me));
    EXPECT_FALSE(mcpd::socketOwnedBy(path, me + 1));

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    EXPECT_TRUE(mcpd::peerUidIs(fds[0], me));
    EXPECT_FALSE(mcpd::peerUidIs(fds[0], me + 1));
    EXPECT_FALSE(mcpd::peerUidIs(-1, me)) << "an unreadable peer must fail closed";
    ::close(fds[0]);
    ::close(fds[1]);
}

// INV-13 — a migration hold taken in this process refuses ants-mcpd's write.
TEST(StandaloneMcpServer, Inv13HoldIsSeenAcrossProcesses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = seedMigratedProject(tmp);
    ASSERT_FALSE(root.isEmpty());
    McpdSession mcpd(root, deadSocket(tmp));
    ASSERT_TRUE(mcpd.started());

    const QString key = RemoteControl::roadmapWriterRoot(root);
    ASSERT_TRUE(RemoteControl::tryHoldRoadmapExclusive(key, 0));
    const QJsonObject busy = mcpd.call(QStringLiteral("roadmap_log"),
                                       appendArgs(root, QStringLiteral("While held.")));
    RemoteControl::releaseRoadmapExclusive(key);
    EXPECT_EQ(busy.value(QStringLiteral("code")).toString(), QStringLiteral("roadmap_busy"))
        << QJsonDocument(busy).toJson().toStdString();

    const QJsonObject ok = mcpd.call(QStringLiteral("roadmap_log"),
                                     appendArgs(root, QStringLiteral("After release.")));
    EXPECT_TRUE(ok.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(ok).toJson().toStdString();
}
