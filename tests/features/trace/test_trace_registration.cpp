// ANTS-5734 § 3, § 7 — how `trace` reaches a client. INV-13: tools/list
// carries `trace` only where mcp::registerMcpdOnlyVerbs ran, and there it
// meets the always-loaded verbs' wire limits (mcp_eager_load). INV-17: the
// built ants-mcpd carries the vendored runtime and links no shared
// tree-sitter. INV-21: `ants-mcpd --call trace` reaches the verb.
// Contract: docs/specs/ANTS-5734-trace-verb.md.

#include "claudeintegration.h"
#include "mcptoolregistry.h"
#include "trace/traceverb.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#if !defined(TRACE_FIXTURE_DIR) || !defined(ANTS_MCPD_BIN)
#error "test_trace_registration needs TRACE_FIXTURE_DIR and ANTS_MCPD_BIN"
#endif

namespace {

QJsonArray toolsList(ClaudeIntegration &ci) {
    QByteArray line;
    McpReplyChannel out([&line](const QByteArray &l) { line = l; }, [] {},
                        [] { return true; });
    ci.handleMcpLine(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})", &out);
    return QJsonDocument::fromJson(line).object().value(QStringLiteral("result"))
        .toObject().value(QStringLiteral("tools")).toArray();
}

std::optional<QJsonObject> toolNamed(const QJsonArray &tools, const QString &name) {
    for (const QJsonValue &v : tools)
        if (v.toObject().value(QStringLiteral("name")).toString() == name) return v.toObject();
    return std::nullopt;
}

// Runs `program args`, returning {exit code, stdout}. -1 when it did not start.
std::pair<int, QByteArray> run(const QString &program, const QStringList &args,
                               const QString &cwd = {},
                               const QProcessEnvironment &env =
                                   QProcessEnvironment::systemEnvironment()) {
    QProcess p;
    p.setProcessEnvironment(env);
    if (!cwd.isEmpty()) p.setWorkingDirectory(cwd);
    p.start(program, args);
    if (!p.waitForStarted(10000)) return {-1, {}};
    p.closeWriteChannel();
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished();
        return {-2, {}};
    }
    return {p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -3, p.readAllStandardOutput()};
}

}  // namespace

// INV-13 — listed and served by a process that registered it, absent from
// one that did not.
TEST(TraceRegistration, Inv13ListedOnlyWhereRegistered) {
    ClaudeIntegration terminal;
    EXPECT_FALSE(toolNamed(toolsList(terminal), QStringLiteral("trace")).has_value())
        << "the terminal's pipeline lists a verb it cannot run";

    ClaudeIntegration mcpd;
    mcp::registerMcpdOnlyVerbs(mcpd, mcp::RegistryHost{});
    EXPECT_TRUE(mcpd.registeredToolNames().contains(QStringLiteral("trace")));
    const auto t = toolNamed(toolsList(mcpd), QStringLiteral("trace"));
    ASSERT_TRUE(t.has_value()) << "registered, but not in tools/list";

    // § 2.8 — always-loaded, so the eager verbs' wire limits apply.
    EXPECT_TRUE(t->value(QStringLiteral("_meta")).toObject()
                    .value(QStringLiteral("anthropic/alwaysLoad")).toBool());
    const QString desc = t->value(QStringLiteral("description")).toString();
    EXPECT_LE(desc.toUtf8().size(), 400) << desc.toStdString();
    EXPECT_TRUE(desc.contains(QStringLiteral("tool_info {name:\"trace\"}"))) << desc.toStdString();
    const QJsonObject schema = t->value(QStringLiteral("inputSchema")).toObject();
    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    for (auto it = props.constBegin(); it != props.constEnd(); ++it)
        EXPECT_LE(it.value().toObject().value(QStringLiteral("description")).toString().size(), 140)
            << it.key().toStdString();
    // § 2.1's arguments, plus `fields`, which tools/list adds to every verb
    // (ANTS-4524), and no others.
    const QSet<QString> expected{QStringLiteral("caller_cwd"), QStringLiteral("symbol"),
                                 QStringLiteral("path"),       QStringLiteral("lang"),
                                 QStringLiteral("include"),    QStringLiteral("depth"),
                                 QStringLiteral("max_bytes"),  QStringLiteral("offset"),
                                 QStringLiteral("fields")};
    const QStringList keys = props.keys();
    EXPECT_EQ(QSet<QString>(keys.begin(), keys.end()), expected);
    EXPECT_EQ(ClaudeIntegration::callerCwdContractFor(QStringLiteral("trace")),
              ClaudeIntegration::CallerCwdContract::Required);
}

// INV-17 — the vendored runtime is linked in; no shared libtree-sitter.
TEST(TraceRegistration, Inv17McpdCarriesTheVendoredRuntime) {
    const QString bin = QStringLiteral(ANTS_MCPD_BIN);
    ASSERT_TRUE(QFileInfo(bin).isExecutable()) << bin.toStdString();
    const QString nm = QStandardPaths::findExecutable(QStringLiteral("nm"));
    const QString ldd = QStandardPaths::findExecutable(QStringLiteral("ldd"));
    ASSERT_FALSE(nm.isEmpty()) << "nm is not installed; INV-17 cannot run";
    ASSERT_FALSE(ldd.isEmpty()) << "ldd is not installed; INV-17 cannot run";

    const auto [nmRc, symbols] = run(nm, {bin});
    ASSERT_EQ(nmRc, 0);
    // Defined in the binary's text, global (T) or local (t): which one
    // depends on the linker, and either means the vendored runtime is in.
    int defined = 0;
    QByteArray seen;
    for (const QByteArray &line : symbols.split('\n')) {
        if (!line.contains("ts_parser_new")) continue;
        seen += line + '\n';
        if (line.endsWith(" T ts_parser_new") || line.endsWith(" t ts_parser_new")) ++defined;
    }
    EXPECT_EQ(defined, 1) << "ts_parser_new is not defined in ants-mcpd; nm shows:\n"
                          << (seen.isEmpty() ? QByteArray("(nothing)") : seen).toStdString();

    const auto [lddRc, libs] = run(ldd, {bin});
    ASSERT_EQ(lddRc, 0);
    EXPECT_FALSE(libs.contains("tree-sitter")) << libs.toStdString();
}

// INV-21 — the script form reaches trace.
TEST(TraceRegistration, Inv21ScriptFormReachesTrace) {
    QTemporaryDir project, data;
    ASSERT_TRUE(project.isValid() && data.isValid());
    const QString src = QStringLiteral(TRACE_FIXTURE_DIR);
    QDirIterator it(src, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString from = it.next();
        const QString to = project.path() + from.mid(src.size());
        QDir().mkpath(QFileInfo(to).absolutePath());
        ASSERT_TRUE(QFile::copy(from, to));
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_DATA_HOME"), data.path());
    env.remove(QStringLiteral("ANTS_OSC133_KEY"));

    const auto [rc, out] =
        run(QStringLiteral(ANTS_MCPD_BIN),
            {QStringLiteral("--call"), QStringLiteral("trace"),
             QStringLiteral(R"({"symbol":"spanFn","include":["definitions"]})")},
            project.path(), env);
    ASSERT_EQ(rc, 0) << out.toStdString();
    const QJsonObject reply = QJsonDocument::fromJson(out).object();
    ASSERT_TRUE(reply.value(QStringLiteral("ok")).toBool()) << out.toStdString();
    const QJsonArray defs = reply.value(QStringLiteral("definitions")).toArray();
    ASSERT_EQ(defs.size(), 1) << out.toStdString();
    EXPECT_EQ(defs.at(0).toObject().value(QStringLiteral("end_line")).toInt(), 9);
}
