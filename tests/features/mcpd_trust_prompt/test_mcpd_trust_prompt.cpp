// ANTS-5464 — ants-mcpd asks the terminal to prompt for trust in a project's
// `.ants/verify.json`. Contract: spec.md here, which cites
// docs/specs/ANTS-5464-mcpd-trust-prompt.md § 3.

#include "../standalone_mcp_server/mcpd_session.h"
#include "../standalone_mcp_server/stub_terminal.h"
#include "../../_support/xdg_guard.h"

#include "claudeintegration.h"
#include "mcpdtrustclient.h"
#include "mcptoolregistry.h"
#include "remotecontrol.h"
#include "rootprovider.h"
#include "verifytrust.h"
#include "verifytrustprompt.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocalServer>
#include <QSemaphore>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

using ants_test::McpdSession;
using ants_test::StubTerminal;

namespace {

QString sha256(const QByteArray &bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray compact(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// A project whose `.ants/verify.json` runs `true` as its build gate. It has
// no build system, so auto-detect finds nothing to run.
struct Project {
    QString root;       // canonical
    QByteArray config;  // the bytes on disk
};

Project makeProject(const QTemporaryDir &tmp, const QString &name) {
    const QString raw = tmp.filePath(name);
    if (!QDir().mkpath(raw + QStringLiteral("/.ants"))) return {};
    Project p;
    p.root = QFileInfo(raw).canonicalFilePath();
    p.config = QByteArray(R"({"build":{"command":"true # )") + name.toUtf8()
               + QByteArray(R"(","format":"plain"}})");
    QFile f(p.root + QStringLiteral("/.ants/verify.json"));
    if (!f.open(QIODevice::WriteOnly) || f.write(p.config) != p.config.size())
        return {};
    return p;
}

// ants-mcpd's trust file starts empty: XDG_CONFIG_HOME is a fresh directory,
// and the auto-trust bypass is off.
struct TrustSandbox {
    ants_test::XdgGuard env;
    QString trustFile;
    explicit TrustSandbox(const QTemporaryDir &tmp) {
        const QString cfg = tmp.filePath(QStringLiteral("config"));
        QDir().mkpath(cfg + QStringLiteral("/Ants Terminal"));
        env.setEnv("XDG_CONFIG_HOME", cfg.toUtf8());
        env.unsetEnv("ANTS_VERIFY_TRUST_AUTOTRUST");
        // QStandardPaths::AppConfigLocation for ants-mcpd, which sets the
        // application name and no organisation.
        trustFile = cfg + QStringLiteral("/Ants Terminal/verify-trust.json");
    }
};

QString dump(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Pumps the event loop until `done` or the deadline.
template <typename Pred>
bool pumpUntil(Pred done, int timeoutMs) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// One request through an in-process pipeline; its reply line is written into
// `*line`, now or later.
struct InProcessCall {
    std::shared_ptr<QByteArray> line = std::make_shared<QByteArray>();
    std::unique_ptr<McpReplyChannel> out;
    InProcessCall(ClaudeIntegration &ci, const QJsonObject &req) {
        auto l = line;
        out = std::make_unique<McpReplyChannel>(
            [l](const QByteArray &bytes) { *l = bytes; }, [] {}, [] { return true; });
        ci.handleMcpLine(compact(req), out.get());
    }
    bool answered() const { return !line->isEmpty(); }
    QJsonObject reply() const { return QJsonDocument::fromJson(*line).object(); }
};

QJsonObject promptRequest(int id, const QJsonObject &params) {
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id},
                       {"method", VerifyTrust::kPromptMethod}, {"params", params}};
}

// Records what the terminal-side handler hands its trust client.
class RecordingClient : public VerifyTrust::Client {
public:
    std::atomic<int> calls{0};
    QString path;
    QByteArray bytes;
    VerifyTrust::Decision outcomeForConfig(const QString &projectPath,
                                           const QByteArray &configBytes) override {
        path = projectPath;
        bytes = configBytes;
        ++calls;
        return {VerifyTrust::Outcome::Trusted, sha256(configBytes)};
    }
    bool addTrustedSha(const QString &, const QString &) override { return true; }
    bool addTrustedRepo(const QString &, const QString &, bool) override { return true; }
    void clearSessionCache() override {}
};

}  // namespace

// INV-1 — the terminal decides from bytes it read itself.
TEST(McpdTrustPrompt, Inv1TerminalDecidesFromItsOwnRead) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());

    ClaudeIntegration ci;
    RecordingClient fake;
    ci.setVerifyTrustPromptHandler([&fake](const QJsonObject &params) {
        return VerifyTrust::answerPromptRequest(params, &fake);
    });

    // Bogus `sha` and `config` beside the root: both must be ignored.
    InProcessCall call(ci, promptRequest(1, QJsonObject{
        {"root", p.root},
        {"sha", QStringLiteral("00").repeated(32)},
        {"config", R"({"build":{"command":"echo pwned"}})"}}));
    ASSERT_TRUE(pumpUntil([&] { return call.answered(); }, 10000));
    const QJsonObject result = call.reply().value(QStringLiteral("result")).toObject();
    EXPECT_EQ(result.value(QStringLiteral("outcome")).toString(), QStringLiteral("trusted"))
        << dump(call.reply()).toStdString();
    EXPECT_EQ(result.value(QStringLiteral("sha")).toString(), sha256(p.config));
    EXPECT_EQ(fake.calls.load(), 1);
    EXPECT_EQ(fake.bytes, p.config) << "the client was not given the bytes on disk";
    EXPECT_EQ(fake.path, p.root);

    // A verify.json that is a symlink out of the root is no config at all,
    // and nobody is asked.
    const QString outside = tmp.filePath(QStringLiteral("outside.json"));
    QFile o(outside);
    ASSERT_TRUE(o.open(QIODevice::WriteOnly));
    o.write(p.config);
    o.close();
    const QString raw2 = tmp.filePath(QStringLiteral("proj2"));
    ASSERT_TRUE(QDir().mkpath(raw2 + QStringLiteral("/.ants")));
    ASSERT_TRUE(QFile::link(outside, raw2 + QStringLiteral("/.ants/verify.json")));
    InProcessCall linked(ci, promptRequest(2, QJsonObject{
        {"root", QFileInfo(raw2).canonicalFilePath()}}));
    ASSERT_TRUE(pumpUntil([&] { return linked.answered(); }, 10000));
    const QJsonObject r2 = linked.reply().value(QStringLiteral("result")).toObject();
    EXPECT_EQ(r2.value(QStringLiteral("outcome")).toString(), QStringLiteral("no_config"))
        << dump(linked.reply()).toStdString();
    EXPECT_TRUE(r2.value(QStringLiteral("sha")).toString().isEmpty());
    EXPECT_EQ(fake.calls.load(), 1) << "the client was asked about a config outside the root";
}

// INV-2 — the terminal's `trusted` alone trusts nothing.
TEST(McpdTrustPrompt, Inv2TerminalWordAloneTrustsNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    TrustSandbox sandbox(tmp);
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());
    StubTerminal stub(tmp.filePath(QStringLiteral("terminal.sock")));
    ASSERT_TRUE(stub.listening());
    QJsonObject seenParams;
    stub.setTrustPromptHandler([&](const QJsonObject &params) -> std::optional<QJsonObject> {
        seenParams = params;
        return QJsonObject{{"outcome", "trusted"}, {"sha", sha256(p.config)}};
    });

    McpdSession mcpd(p.root, stub.path());
    ASSERT_TRUE(mcpd.started());
    const QJsonObject resp = mcpd.call(QStringLiteral("verify_changes"),
                                       QJsonObject{{"caller_cwd", p.root}}, 30000);
    ASSERT_FALSE(resp.value(QStringLiteral("test_timeout")).toBool()) << dump(resp).toStdString();
    EXPECT_EQ(stub.trustPromptCount(), 1) << "the terminal was not asked exactly once";
    EXPECT_EQ(seenParams.value(QStringLiteral("root")).toString(), p.root);
    EXPECT_TRUE(resp.value(QStringLiteral("verify_untrusted")).toBool())
        << "a `trusted` reply with nothing in the trust file was honoured: "
        << dump(resp).toStdString();
}

// INV-3 — a grant runs the project's own gates in the same call.
TEST(McpdTrustPrompt, Inv3GrantRunsTheGatesInTheSameCall) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    TrustSandbox sandbox(tmp);
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());
    StubTerminal stub(tmp.filePath(QStringLiteral("terminal.sock")));
    ASSERT_TRUE(stub.listening());
    const QString trustFile = sandbox.trustFile;
    stub.setTrustPromptHandler([&](const QJsonObject &) -> std::optional<QJsonObject> {
        // What the terminal's dialog does on "Trust this SHA".
        VerifyTrust::FilePersistedTrustClient(trustFile).addTrustedSha(sha256(p.config));
        return QJsonObject{{"outcome", "trusted"}, {"sha", sha256(p.config)}};
    });

    McpdSession mcpd(p.root, stub.path());
    ASSERT_TRUE(mcpd.started());
    const QJsonObject resp = mcpd.call(QStringLiteral("verify_changes"),
                                       QJsonObject{{"caller_cwd", p.root}}, 30000);
    ASSERT_FALSE(resp.value(QStringLiteral("test_timeout")).toBool()) << dump(resp).toStdString();
    EXPECT_EQ(stub.trustPromptCount(), 1);
    ASSERT_TRUE(QFileInfo::exists(trustFile)) << "the stub's grant never reached "
                                              << trustFile.toStdString();
    EXPECT_EQ(resp.value(QStringLiteral("config_source")).toString(),
              QStringLiteral(".ants/verify.json")) << dump(resp).toStdString();
    EXPECT_FALSE(resp.value(QStringLiteral("verify_untrusted")).toBool())
        << dump(resp).toStdString();
}

// INV-4 — with no terminal, the fallback comes at once.
TEST(McpdTrustPrompt, Inv4NoTerminalFallsBackWithoutWaiting) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    TrustSandbox sandbox(tmp);
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());

    McpdSession mcpd(p.root, tmp.filePath(QStringLiteral("no-terminal.sock")));
    ASSERT_TRUE(mcpd.started());
    // Measured from after the handshake, so start-up cost is not counted.
    ASSERT_FALSE(mcpd.await(1, 20000).contains(QStringLiteral("test_timeout")));
    QElapsedTimer t;
    t.start();
    const QJsonObject resp = mcpd.call(QStringLiteral("verify_changes"),
                                       QJsonObject{{"caller_cwd", p.root}}, 20000);
    const qint64 ms = t.elapsed();
    ASSERT_FALSE(resp.value(QStringLiteral("test_timeout")).toBool()) << dump(resp).toStdString();
    EXPECT_TRUE(resp.value(QStringLiteral("verify_untrusted")).toBool()) << dump(resp).toStdString();
    EXPECT_LT(ms, 5000) << "verify_changes waited " << ms << " ms with no terminal";
}

// INV-5 — a denial is asked once per SHA per ants-mcpd process.
TEST(McpdTrustPrompt, Inv5DenialIsAskedOncePerSha) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    TrustSandbox sandbox(tmp);
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());
    StubTerminal stub(tmp.filePath(QStringLiteral("terminal.sock")));
    ASSERT_TRUE(stub.listening());
    stub.setTrustPromptHandler([&](const QJsonObject &) -> std::optional<QJsonObject> {
        return QJsonObject{{"outcome", "denied"}, {"sha", sha256(p.config)}};
    });

    McpdSession mcpd(p.root, stub.path());
    ASSERT_TRUE(mcpd.started());
    for (int i = 0; i < 2; ++i) {
        const QJsonObject resp = mcpd.call(QStringLiteral("verify_changes"),
                                           QJsonObject{{"caller_cwd", p.root}}, 30000);
        ASSERT_FALSE(resp.value(QStringLiteral("test_timeout")).toBool()) << dump(resp).toStdString();
        EXPECT_TRUE(resp.value(QStringLiteral("verify_untrusted")).toBool()) << dump(resp).toStdString();
    }
    EXPECT_EQ(stub.trustPromptCount(), 1) << "a denial was not remembered for its SHA";
}

// INV-6 — a late or missing answer is Headless, and is asked again.
TEST(McpdTrustPrompt, Inv6TimeoutIsHeadlessAndNotCached) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());

    // Accepts nothing and answers nothing: a connection waits in the backlog.
    QLocalServer silent;
    const QString sock = tmp.filePath(QStringLiteral("silent.sock"));
    ASSERT_TRUE(silent.listen(sock));
    ants_test::XdgGuard env;
    env.setEnv("ANTS_MCP_SOCKET", sock.toUtf8());

    mcpd::ForwardingTrustClient client(tmp.filePath(QStringLiteral("trust.json")), 200);
    for (int i = 0; i < 2; ++i) {
        QElapsedTimer t;
        t.start();
        const VerifyTrust::Decision d = client.outcomeForConfig(p.root, p.config);
        EXPECT_EQ(d.outcome, VerifyTrust::Outcome::Headless) << "lookup " << i;
        EXPECT_LT(t.elapsed(), 2000) << "lookup " << i;
    }
    int connections = 0;
    QObject::connect(&silent, &QLocalServer::newConnection, &silent, [&] {
        while (QLocalSocket *s = silent.nextPendingConnection()) {
            ++connections;
            s->deleteLater();
        }
    });
    pumpUntil([&] { return connections >= 2; }, 2000);
    EXPECT_EQ(connections, 2) << "the terminal was not asked on each lookup";
}

// INV-7 — the method is not a tool, and a host without a handler refuses it.
TEST(McpdTrustPrompt, Inv7MethodIsNotATool) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());
    const auto names = [](const QJsonObject &reply) {
        QStringList out;
        const QJsonArray tools = reply.value(QStringLiteral("result")).toObject()
                                     .value(QStringLiteral("tools")).toArray();
        for (const QJsonValue v : tools)
            out << v.toObject().value(QStringLiteral("name")).toString();
        return out;
    };

    McpdSession mcpd(p.root, tmp.filePath(QStringLiteral("no-terminal.sock")));
    ASSERT_TRUE(mcpd.started());
    const QStringList remote = names(mcpd.await(mcpd.send(QStringLiteral("tools/list"))));
    ASSERT_FALSE(remote.isEmpty());
    EXPECT_FALSE(remote.contains(QLatin1String(VerifyTrust::kPromptMethod)));

    ClaudeIntegration ci;
    InProcessCall list(ci, QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    ASSERT_TRUE(pumpUntil([&] { return list.answered(); }, 10000));
    const QStringList local = names(list.reply());
    ASSERT_FALSE(local.isEmpty());
    EXPECT_FALSE(local.contains(QLatin1String(VerifyTrust::kPromptMethod)));

    const QJsonObject refused = mcpd.await(mcpd.send(
        QLatin1String(VerifyTrust::kPromptMethod), QJsonObject{{"root", p.root}}));
    EXPECT_EQ(refused.value(QStringLiteral("error")).toObject()
                  .value(QStringLiteral("code")).toInt(), -32601)
        << dump(refused).toStdString();
}

// INV-8 — ants-mcpd keeps answering while a prompt is pending.
TEST(McpdTrustPrompt, Inv8McpdAnswersWhileAPromptIsPending) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    TrustSandbox sandbox(tmp);
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());
    ASSERT_TRUE(QDir().mkpath(p.root + QStringLiteral("/docs/specs")));
    QFile spec(p.root + QStringLiteral("/docs/specs/T-1-demo.md"));
    ASSERT_TRUE(spec.open(QIODevice::WriteOnly));
    spec.write("# T-1 Demo\n\n## Invariants\n\n- **INV-1** — a rule.\n");
    spec.close();

    StubTerminal stub(tmp.filePath(QStringLiteral("terminal.sock")));
    ASSERT_TRUE(stub.listening());
    stub.setTrustPromptHandler([](const QJsonObject &) -> std::optional<QJsonObject> {
        return std::nullopt;   // the user has not answered yet
    });

    McpdSession mcpd(p.root, stub.path());
    ASSERT_TRUE(mcpd.started());
    const int verifyId = mcpd.sendCall(QStringLiteral("verify_changes"),
                                       QJsonObject{{"caller_cwd", p.root}});
    // Whatever happens below, the held prompt is answered before teardown.
    struct Release {
        StubTerminal &s;
        ~Release() { s.releaseHeld(QJsonObject{{"outcome", "headless"}, {"sha", ""}}); }
    } release{stub};

    QElapsedTimer t;
    t.start();
    while (stub.trustPromptCount() == 0 && t.elapsed() < 20000)
        mcpd.await(-1, 50);
    ASSERT_EQ(stub.trustPromptCount(), 1) << "the terminal was never asked";

    const QJsonObject list = mcpd.await(mcpd.send(QStringLiteral("tools/list")), 10000);
    EXPECT_FALSE(list.contains(QStringLiteral("test_timeout"))) << dump(list).toStdString();
    const QJsonObject lint = mcpd.call(QStringLiteral("spec_lint"),
        QJsonObject{{"caller_cwd", p.root}, {"path", "docs/specs/T-1-demo.md"}}, 20000);
    EXPECT_FALSE(lint.contains(QStringLiteral("test_timeout"))) << dump(lint).toStdString();
    EXPECT_FALSE(mcpd.hasReply(verifyId)) << "verify_changes finished before the prompt was answered";

    stub.releaseHeld(QJsonObject{{"outcome", "headless"}, {"sha", ""}});
    const QJsonObject verify = mcpd.await(verifyId, 30000);
    EXPECT_FALSE(verify.contains(QStringLiteral("test_timeout"))) << dump(verify).toStdString();
}

// INV-9 — the terminal keeps answering while its handler is open.
TEST(McpdTrustPrompt, Inv9TerminalAnswersWhileItsHandlerIsOpen) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const Project p = makeProject(tmp, QStringLiteral("proj"));
    ASSERT_FALSE(p.root.isEmpty());
    ASSERT_TRUE(QDir().mkpath(p.root + QStringLiteral("/docs/specs")));
    QFile spec(p.root + QStringLiteral("/docs/specs/T-1-demo.md"));
    ASSERT_TRUE(spec.open(QIODevice::WriteOnly));
    spec.write("# T-1 Demo\n\n## Invariants\n\n- **INV-1** — a rule.\n");
    spec.close();

    ClaudeIntegration ci;
    ants::ServerCwdRootProvider roots;
    RemoteControl rc(nullptr, nullptr, &roots);
    mcp::RegistryHost host;
    host.ci = &ci;
    host.roots = &roots;
    mcp::registerProjectScopedVerbs(ci, [&rc] { return &rc; }, host);

    auto latch = std::make_shared<QSemaphore>(0);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    ci.setVerifyTrustPromptHandler([latch, entered](const QJsonObject &) {
        entered->store(true);
        latch->tryAcquire(1, 20000);
        return QJsonObject{{"outcome", "headless"}, {"sha", ""}};
    });
    // Released on every exit, so the worker is never left blocked at teardown.
    struct Release {
        std::shared_ptr<QSemaphore> l;
        ~Release() { l->release(); }
    } release{latch};

    InProcessCall prompt(ci, promptRequest(1, QJsonObject{{"root", p.root}}));
    ASSERT_TRUE(pumpUntil([&] { return entered->load(); }, 10000))
        << "the handler never ran: " << prompt.line->toStdString();

    InProcessCall list(ci, QJsonObject{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
    EXPECT_TRUE(pumpUntil([&] { return list.answered(); }, 10000));
    InProcessCall lint(ci, QJsonObject{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
        {"params", QJsonObject{{"name", "spec_lint"}, {"arguments", QJsonObject{
            {"caller_cwd", p.root}, {"path", "docs/specs/T-1-demo.md"}}}}}});
    EXPECT_TRUE(pumpUntil([&] { return lint.answered(); }, 20000));
    EXPECT_FALSE(prompt.answered()) << "the prompt was answered before the latch opened";

    latch->release();
    EXPECT_TRUE(pumpUntil([&] { return prompt.answered(); }, 10000));
}
