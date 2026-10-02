// ANTS-5236 — the Claude hook and MCP sockets bind in the private runtime
// directory, never in the world-writable /tmp.
//
// Why this exists: both sockets were bound at guessable /tmp names, so another
// local user could squat the name, and every client had to defend itself.
//
// Each case points XDG_RUNTIME_DIR (A, mode 0700) and TMPDIR (B) at separate
// scratch directories, so a path left in tempPath() cannot pass as a runtime
// path. INV-1 and INV-9 also scrape mainwindow.cpp, because they are claims
// about where code sits. Contract: spec.md beside this file, and
// docs/specs/ANTS-5236-sockets-in-runtime-dir.md.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../_support/srcgrep.h"
#include "claudeintegration.h"
#include "claudesetup.h"
#include "configpaths.h"
#include "mcpdsocket.h"
#include "secureio.h"

#ifndef ANTS_SOURCE_DIR
#  error "ANTS_SOURCE_DIR compile definition required"
#endif

namespace {

bool waitUntil(const std::function<bool()> &done, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done();
}

// Saves the named variables, restores them on scope exit.
class EnvGuard {
public:
    EnvGuard() {
        for (const char *n : kNames)
            m_saved.push_back({n, qEnvironmentVariableIsSet(n), qgetenv(n)});
    }
    ~EnvGuard() {
        for (const auto &s : m_saved) {
            if (s.wasSet) qputenv(s.name, s.value);
            else          qunsetenv(s.name);
        }
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

private:
    struct Saved { const char *name; bool wasSet; QByteArray value; };
    static constexpr const char *kNames[] = {
        "XDG_RUNTIME_DIR", "TMPDIR", "HOME", "ANTS_MCP_SOCKET",
        "ANTS_CLAUDE_HOOK_SOCKET"};
    std::vector<Saved> m_saved;
};

// A = runtime directory (0700), B = tempPath(), both inside one scratch dir.
struct Sandbox {
    EnvGuard       env;
    QTemporaryDir  base;
    QString        a;   // XDG_RUNTIME_DIR
    QString        b;   // TMPDIR
    QString        home;

    Sandbox() {
        if (!base.isValid()) return;
        a    = base.path() + QStringLiteral("/run");
        b    = base.path() + QStringLiteral("/tmp");
        home = base.path() + QStringLiteral("/home");
        QDir().mkpath(a);
        QDir().mkpath(b);
        QDir().mkpath(home);
        ::chmod(QFile::encodeName(a).constData(), 0700);
        qputenv("XDG_RUNTIME_DIR", a.toLocal8Bit());
        qputenv("TMPDIR", b.toLocal8Bit());
        qputenv("HOME", home.toLocal8Bit());
        qunsetenv("ANTS_MCP_SOCKET");
        qunsetenv("ANTS_CLAUDE_HOOK_SOCKET");
    }
    bool ok() const { return base.isValid(); }
    QString antsDir() const { return a + QStringLiteral("/ants-terminal"); }
    void makeAntsDir(mode_t mode) const {
        QDir().mkpath(antsDir());
        ::chmod(QFile::encodeName(antsDir()).constData(), mode);
    }
};

// Comma-joined listing, so a failure prints what was there.
QString entriesOf(const QString &dir) {
    return QDir(dir).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System | QDir::Hidden)
        .join(QLatin1Char(','));
}

// A socket file with nobody listening: what a crashed terminal leaves behind.
bool makeLeftoverSocket(const QString &path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray p = QFile::encodeName(path);
    if (p.size() >= static_cast<int>(sizeof(addr.sun_path))) { ::close(fd); return false; }
    std::memcpy(addr.sun_path, p.constData(), static_cast<size_t>(p.size()));
    const int rc = ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    ::close(fd);
    return rc == 0;
}

// Listens on `path` and gathers every byte any client sends.
class Collector {
public:
    bool listen(const QString &path) {
        QLocalServer::removeServer(path);
        QObject::connect(&m_server, &QLocalServer::newConnection, [this] {
            while (QLocalSocket *c = m_server.nextPendingConnection()) {
                QObject::connect(c, &QLocalSocket::readyRead, c,
                                 [this, c] { m_data += c->readAll(); });
                QObject::connect(c, &QLocalSocket::disconnected, c,
                                 &QObject::deleteLater);
            }
        });
        return m_server.listen(path);
    }
    QByteArray data() const { return m_data; }
    void close() { m_server.close(); }
private:
    QLocalServer m_server;
    QByteArray   m_data;
};

bool haveTool(const char *name) {
    return !QStandardPaths::findExecutable(QString::fromLatin1(name)).isEmpty();
}

}  // namespace

// INV-1
TEST(ClaudeSocketRuntimeDir, Inv1PathsUseRuntimeDir) {
    Sandbox sb;
    ASSERT_TRUE(sb.ok());
    ASSERT_EQ(ConfigPaths::antsRuntimeDir(), sb.antsDir())
        << "precondition: Qt's RuntimeLocation must follow XDG_RUNTIME_DIR";

    const QString want = sb.antsDir() + QStringLiteral("/claude-hooks-")
                       + QString::number(::getpid());
    EXPECT_EQ(ClaudeIntegration::defaultHookSocketPath(), want)
        << "expected the hook socket under antsRuntimeDir(); actual path "
           "shows where defaultHookSocketPath() binds (tempPath is "
        << QDir::tempPath().toStdString() << ")";

    const std::string all = ants_test::slurpMainWindow();
    ASSERT_FALSE(all.empty()) << "could not read the MainWindow sources";
    const std::string body = ants_test::slurpFunctionBody(
        all, "void MainWindow::setupClaudeMcpProviders()");
    ASSERT_FALSE(body.empty()) << "MainWindow::setupClaudeMcpProviders not found";
    // Anchor: `mcpSocket = privateSocketPath(... "mcp-" ...);` — tolerant of
    // line breaks, QStringLiteral and a namespace prefix.
    const QRegularExpression rx(
        QStringLiteral(R"(mcpSocket\s*=\s*[\w:]*privateSocketPath\([^;]*"mcp-)"),
        QRegularExpression::DotMatchesEverythingOption);
    EXPECT_TRUE(rx.match(QString::fromStdString(body)).hasMatch())
        << "expected `mcpSocket = privateSocketPath(\"mcp-\" ...)` in "
           "setupClaudeMcpProviders; the MCP socket is still built elsewhere";
}

// INV-2
TEST(ClaudeSocketRuntimeDir, Inv2BadDirBindsNothing) {
    Sandbox sb;
    ASSERT_TRUE(sb.ok());
    sb.makeAntsDir(0755);

    EXPECT_EQ(privateSocketPath(QStringLiteral("x")), QString())
        << "expected empty: the runtime dir exists at mode 0755, so "
           "ensureSocketDir must refuse it";

    ClaudeIntegration c;
    EXPECT_FALSE(c.startHookServer(QString()))
        << "startHookServer must fail for an empty path";
    EXPECT_FALSE(c.startMcpServer(QString()))
        << "startMcpServer must fail for an empty path";
    EXPECT_EQ(entriesOf(sb.b), QString())
        << "nothing may be created under tempPath(): no /tmp fallback";
}

// INV-3
TEST(ClaudeSocketRuntimeDir, Inv3PickerIgnoresLegacyName) {
    Sandbox sb;
    ASSERT_TRUE(sb.ok());
    const QString pid = QString::number(::getpid());

    {   // (a) a live socket in the runtime directory
        sb.makeAntsDir(0700);
        const QString path = sb.antsDir() + QStringLiteral("/mcp-") + pid;
        Collector srv;
        ASSERT_TRUE(srv.listen(path));
        QString why;
        EXPECT_EQ(mcpd::pickTerminalSocket(::getuid(), &why), path)
            << "(a) runtime-dir socket not picked; whyNot: " << why.toStdString();
        srv.close();
        QFile::remove(path);
    }
    {   // (b) a live socket under the legacy tempPath() name is not picked (ANTS-5587)
        const QString path = sb.b + QStringLiteral("/ants-terminal-mcp-") + pid;
        Collector srv;
        ASSERT_TRUE(srv.listen(path));
        EXPECT_EQ(mcpd::pickTerminalSocket(::getuid()), QString())
            << "(b) a legacy tempPath() socket was picked";
        srv.close();
        QFile::remove(path);
    }
    {   // (c) a regular file at the runtime-dir name is never returned
        const QString path = sb.antsDir() + QStringLiteral("/mcp-1");
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.close();
        const QString got = mcpd::pickTerminalSocket(::getuid());
        EXPECT_NE(got, path) << "(c) a regular file was returned as a socket";
        EXPECT_EQ(got, QString()) << "(c) nothing valid exists, expected empty";
    }
}

// INV-5
TEST(ClaudeSocketRuntimeDir, Inv5ScriptUsesExportedSocketOnly) {
    if (!haveTool("python3") || !haveTool("bash"))
        GTEST_SKIP() << "python3 or bash not installed";
    Sandbox sb;
    ASSERT_TRUE(sb.ok());
    sb.makeAntsDir(0700);

    const QString script = ants::claude_setup::statusHookScript();
    EXPECT_FALSE(script.isEmpty()) << "statusHookScript returned no text";

    // Text checks: the peer-uid compare guards the send, and the path reaches
    // Python as argv, never spliced into its source.
    EXPECT_EQ(script.count(QStringLiteral("s.connect(")), 1)
        << "expected exactly one socket connect";
    const int peer = script.indexOf(QStringLiteral("SO_PEERCRED"));
    const int uid  = script.indexOf(QStringLiteral("getuid"), peer);
    const int send = script.indexOf(QStringLiteral("sendall"));
    EXPECT_TRUE(peer >= 0 && uid > peer && send > uid)
        << "expected SO_PEERCRED, then the uid compare, then sendall; got "
        << peer << "," << uid << "," << send;
    EXPECT_TRUE(script.contains(QStringLiteral("sys.argv")))
        << "the socket path must arrive as argv";
    EXPECT_FALSE(script.contains(QStringLiteral("connect('$")))
        << "the socket path is spliced into the Python source";

    const QString scriptPath = sb.base.path() + QStringLiteral("/forward.sh");
    {
        QFile f(scriptPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(script.toUtf8());
    }
    const QByteArray payload = "{\"hook_event_name\":\"Stop\"}";
    const QString bash = QStandardPaths::findExecutable(QStringLiteral("bash"));

    {   // Route 1: $ANTS_CLAUDE_HOOK_SOCKET
        Collector srv;
        const QString sock = sb.antsDir() + QStringLiteral("/claude-hooks-route1");
        ASSERT_TRUE(srv.listen(sock));
        QProcess p;
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert(QStringLiteral("ANTS_CLAUDE_HOOK_SOCKET"), sock);
        p.setProcessEnvironment(e);
        p.start(bash, {scriptPath});
        ASSERT_TRUE(p.waitForStarted());
        p.write(payload);
        p.closeWriteChannel();
        waitUntil([&] { return p.state() == QProcess::NotRunning; }, 10000);
        waitUntil([&] { return srv.data() == payload; }, 3000);
        EXPECT_EQ(srv.data(), payload)
            << "route 1: stdin not delivered to $ANTS_CLAUDE_HOOK_SOCKET";
    }
    {   // No route 2 (ANTS-5587): a legacy <tempPath>/ants-claude-hooks-<pid> of an
        // ants-terminal ancestor receives nothing when the variable is unset.
        const QString fake = sb.base.path() + QStringLiteral("/ants-terminal");
        ASSERT_TRUE(QFile::copy(bash, fake));
        QFile::setPermissions(fake, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
        QProcess tree;
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.remove(QStringLiteral("ANTS_CLAUDE_HOOK_SOCKET"));
        e.insert(QStringLiteral("L"), sb.b);
        e.insert(QStringLiteral("SCRIPT"), scriptPath);
        e.insert(QStringLiteral("PAYLOAD"), QString::fromLatin1(payload));
        tree.setProcessEnvironment(e);
        tree.start(fake, {QStringLiteral("-c"), QStringLiteral(
            "while [ ! -S \"$L/ants-claude-hooks-$$\" ]; do sleep 0.05; done; "
            "printf '%s' \"$PAYLOAD\" | bash \"$SCRIPT\"")});
        ASSERT_TRUE(tree.waitForStarted());
        const qint64 pid = tree.processId();
        ASSERT_GT(pid, 0);
        Collector srv;
        ASSERT_TRUE(srv.listen(sb.b + QStringLiteral("/ants-claude-hooks-")
                               + QString::number(pid)));
        waitUntil([&] { return tree.state() == QProcess::NotRunning; }, 10000);
        waitUntil([&] { return !srv.data().isEmpty(); }, 500);
        EXPECT_TRUE(srv.data().isEmpty())
            << "stdin was delivered to the legacy "
               "<tempPath>/ants-claude-hooks-<pid> of the ants-terminal ancestor";
        if (tree.state() != QProcess::NotRunning) {
            tree.kill();
            tree.waitForFinished(2000);
        }
    }
}

// INV-6
TEST(ClaudeSocketRuntimeDir, Inv6RefreshOnlyWhatExists) {
    Sandbox sb;
    ASSERT_TRUE(sb.ok());
    const QString file = ConfigPaths::antsClaudeForwardScript();
    ASSERT_TRUE(file.startsWith(sb.home)) << "precondition: HOME drives the path: " << file.toStdString();
    const QString expected = ants::claude_setup::statusHookScript();

    // Absent: nothing is created.
    ants::claude_setup::refreshStatusHookScript();
    EXPECT_FALSE(QFileInfo::exists(file))
        << "refresh must not create the forwarder where it was never installed";
    EXPECT_FALSE(QFileInfo::exists(ConfigPaths::antsHooksDir()))
        << "refresh must not create the hooks directory either";

    // Stale: rewritten to the current text.
    QDir().mkpath(ConfigPaths::antsHooksDir());
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("stale");
    }
    ants::claude_setup::refreshStatusHookScript();
    QFile in(file);
    ASSERT_TRUE(in.open(QIODevice::ReadOnly));
    const QByteArray now = in.readAll();
    in.close();
    EXPECT_FALSE(expected.isEmpty()) << "statusHookScript returned no text";
    EXPECT_EQ(now, expected.toUtf8())
        << "a stale forwarder must be rewritten to statusHookScript()";

    // Up to date: untouched (mtime survives).
    const QDateTime past = QDateTime::currentDateTime().addDays(-30);
    {
        QFile f(file);
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_TRUE(f.setFileTime(past, QFileDevice::FileModificationTime));
    }
    const qint64 before = QFileInfo(file).lastModified().toSecsSinceEpoch();
    ants::claude_setup::refreshStatusHookScript();
    EXPECT_EQ(QFileInfo(file).lastModified().toSecsSinceEpoch(), before)
        << "an up-to-date forwarder must not be rewritten";
}

// INV-7
TEST(ClaudeSocketRuntimeDir, Inv7SweepCoversRuntimeDirOnly) {
    Sandbox sb;
    ASSERT_TRUE(sb.ok());
    sb.makeAntsDir(0700);

    QProcess t;
    t.start(QStringLiteral("true"));
    ASSERT_TRUE(t.waitForStarted());
    const qint64 dead = t.processId();
    ASSERT_TRUE(t.waitForFinished(5000));
    ASSERT_GT(dead, 1);
    ASSERT_NE(::kill(static_cast<pid_t>(dead), 0), 0) << "pid " << dead << " still live";

    const QString deadA = sb.antsDir() + QStringLiteral("/mcp-") + QString::number(dead);
    const QString deadB = sb.b + QStringLiteral("/ants-terminal-mcp-") + QString::number(dead);
    const QString initA = sb.antsDir() + QStringLiteral("/mcp-1");
    const QString selfA = sb.antsDir() + QStringLiteral("/mcp-") + QString::number(::getpid());
    for (const QString &p : {deadA, deadB, initA, selfA})
        ASSERT_TRUE(makeLeftoverSocket(p)) << p.toStdString();

    // ANTS-5080 — a regular file planted under a dead pid's legacy name is not
    // the reaper's to remove. A second dead pid, so the name is free.
    QProcess t2;
    t2.start(QStringLiteral("true"));
    ASSERT_TRUE(t2.waitForStarted());
    const qint64 dead2 = t2.processId();
    ASSERT_TRUE(t2.waitForFinished(5000));
    ASSERT_NE(::kill(static_cast<pid_t>(dead2), 0), 0) << "pid " << dead2 << " still live";
    const QString plantedB = sb.b + QStringLiteral("/ants-terminal-mcp-") + QString::number(dead2);
    {
        QFile f(plantedB);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("not a socket");
    }

    mcpd::reapStaleTerminalSockets(::getpid());

    EXPECT_FALSE(QFileInfo::exists(deadA)) << "dead-pid socket in the runtime dir survived";
    EXPECT_TRUE(QFileInfo::exists(deadB))
        << "a socket under the legacy tempPath() name was swept (ANTS-5587)";
    EXPECT_TRUE(QFileInfo::exists(initA)) << "pid 1 (EPERM, live) was removed";
    EXPECT_TRUE(QFileInfo::exists(selfA)) << "self's socket was removed";
    EXPECT_TRUE(QFileInfo::exists(plantedB))
        << "a regular file under a dead pid's name was removed (ANTS-5080)";
}

// INV-9
TEST(ClaudeSocketRuntimeDir, Inv9HookSocketExported) {
    const std::string all = ants_test::slurpMainWindow();
    ASSERT_FALSE(all.empty()) << "could not read the MainWindow sources";
    const std::string needle = "qputenv(\"ANTS_CLAUDE_HOOK_SOCKET\"";
    size_t n = 0;
    for (size_t p = all.find(needle); p != std::string::npos;
         p = all.find(needle, p + 1))
        ++n;
    EXPECT_EQ(n, 1u) << "expected exactly one " << needle << " in the MainWindow sources";

    const std::string body = ants_test::slurpFunctionBody(
        all, "void MainWindow::setupClaudeMcpProviders()");
    ASSERT_FALSE(body.empty()) << "MainWindow::setupClaudeMcpProviders not found";
    const size_t start = body.find("startHookServer(");
    const size_t put   = body.find(needle);
    EXPECT_NE(start, std::string::npos) << "startHookServer( not in setupClaudeMcpProviders";
    EXPECT_NE(put, std::string::npos) << "the export is not in setupClaudeMcpProviders";
    if (start != std::string::npos && put != std::string::npos) {
        EXPECT_LT(start, put) << "the export must follow the startHookServer call";
    }
}
