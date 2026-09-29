// ANTS-5506 — `ants-mcpd --call <verb> [<json> | -] [--exit-code]`. Contract:
// spec.md here, which cites docs/specs/ANTS-5506-mcpd-call.md § 3.
//
// Why this exists: src/mcpdcall.cpp is a deliberate stub (every function
// returns a value the contract rejects) and src/mcpdmain.cpp does not
// recognise --call at all yet — it falls through to the ordinary stdio
// JSON-RPC loop. This suite is expected to fail its assertions until both
// land; every case still executes and reports what it expected against what
// it saw, rather than hanging or refusing to build.
//
// Every INV-numbered case but INV-4 runs the built ants-mcpd as a child
// process, because what is under test is that binary's --call mode.

#include "../standalone_mcp_server/mcpd_session.h"

#include "mcpdcall.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif
#ifndef ANTS_MCPD_BIN
#error "ANTS_MCPD_BIN compile definition required"
#endif

using ants_test::McpdSession;

namespace {

// A socket path with no listener — every case here is project-scoped, so
// nothing is ever forwarded to a terminal.
QString deadSocket(const QTemporaryDir &tmp) {
    return tmp.filePath(QStringLiteral("no-terminal.sock"));
}

// Writes <configHome>/ants-terminal/config.json with the flat dotted keys
// Config::load() reads directly (src/config.cpp: m_data = doc.object(), no
// nesting). Returns false on any I/O failure.
bool writeConfig(const QString &configHome, const QJsonObject &keys) {
    const QString dir = configHome + QStringLiteral("/ants-terminal");
    if (!QDir().mkpath(dir)) return false;
    QFile f(dir + QStringLiteral("/config.json"));
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray body = QJsonDocument(keys).toJson(QJsonDocument::Compact);
    return f.write(body) == body.size();
}

// A one-file spec_lint fixture, the same shape
// standalone_mcp_server's Inv5NoTerminalRefusesForwardedVerbsOnly uses.
// `clean`: the INV-1 bullet carries a *Test:* clause (0 findings —
// invariant_no_test does not fire). Otherwise it does not (exactly 1
// finding). Neither fixture has a "## Tests" section, so
// test_coverage_gap/unverifiable are skipped (speclint.cpp: "No Tests
// section: the comparison has one half. SKIP"), and neither project carries
// a spec-format standard, so missing_section is skipped too. Returns the
// project's canonical root, or an empty string on failure.
QString writeSpecFixture(const QTemporaryDir &tmp, const QString &name, bool clean) {
    const QString proj = tmp.filePath(name);
    if (!QDir().mkpath(proj + QStringLiteral("/docs/specs"))) return {};
    QFile spec(proj + QStringLiteral("/docs/specs/T-1-demo.md"));
    if (!spec.open(QIODevice::WriteOnly)) return {};
    QByteArray body = "# T-1 Demo\n\n## Invariants\n\n- **INV-1** \xe2\x80\x94 a rule.";
    body += clean ? " *Test:* `tests/features/t1_demo/` covers it.\n" : "\n";
    if (spec.write(body) != body.size()) return {};
    spec.close();
    return QFileInfo(proj).canonicalFilePath();
}

// A doc_lint fixture with nothing to flag: no links, no citations, no
// duplicated passages, no backticked identifiers, no ToC. No docs/specs/
// under it either, so spec_lint's sub-check (doc_lint composes five
// checkers) enumerates zero documents rather than finding anything.
QString writeDocLintCleanFixture(const QTemporaryDir &tmp, const QString &name) {
    const QString proj = tmp.filePath(name);
    if (!QDir().mkpath(proj + QStringLiteral("/docs"))) return {};
    QFile f(proj + QStringLiteral("/docs/README.md"));
    if (!f.open(QIODevice::WriteOnly)) return {};
    const QByteArray body = "# Demo\n\nA short clean document with nothing to flag.\n";
    if (f.write(body) != body.size()) return {};
    return QFileInfo(proj).canonicalFilePath();
}

QString compactJson(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

struct CallOutcome {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QByteArray out;
    QByteArray err;
};

// Spawns the built ants-mcpd with `args`. `closeStdin=false` is INV-5's
// case: stdin is left open across the whole wait. On a timeout the process
// is killed so the test does not leak a hung child into the rest of the run.
CallOutcome runMcpdCall(const QStringList &args, const QString &cwd,
                        const QProcessEnvironment &env,
                        bool closeStdin = true, int timeoutMs = 20000) {
    QProcess p;
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(cwd);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral(ANTS_MCPD_BIN), args);
    CallOutcome r;
    r.started = p.waitForStarted(5000);
    if (!r.started) {
        r.err = "ants-mcpd failed to start";
        return r;
    }
    if (closeStdin) p.closeWriteChannel();
    r.finished = p.waitForFinished(timeoutMs);
    if (!r.finished) {
        p.kill();
        p.waitForFinished(2000);
    }
    r.exitCode = p.exitCode();
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    return r;
}

}  // namespace

// INV-1 — stdout is the same envelope the stdio pipeline returns, unwrapped,
// when both runs get the same arguments and neither has a reply-shaping key.
// The --call side's OWN config carries terse:true / offload:true — the
// opposite of the stdio reference run's config — so a --call that merely
// inherited its config's settings (rather than forcing both off itself, per
// § 2.1 points 1-2) would produce a visibly different JSON shape (terse
// compaction drops the empty `findings` array) and this comparison would
// catch it.
TEST(McpdCall, Inv1StdoutMatchesStdioUnwrapped) {
    QTemporaryDir tmp, sock, cfgStdio, dataStdio, cfgCall, dataCall;
    ASSERT_TRUE(tmp.isValid() && sock.isValid() && cfgStdio.isValid() &&
                dataStdio.isValid() && cfgCall.isValid() && dataCall.isValid());
    const QString proj = writeSpecFixture(tmp, QStringLiteral("inv1"), /*clean=*/true);
    ASSERT_FALSE(proj.isEmpty());

    ASSERT_TRUE(writeConfig(cfgStdio.path(), QJsonObject{
        {"claude.mcp_terse_responses", false},
        {"claude.mcp_offload_large_results", false}}));
    ASSERT_TRUE(writeConfig(cfgCall.path(), QJsonObject{
        {"claude.mcp_terse_responses", true},
        {"claude.mcp_offload_large_results", true}}));

    const QJsonObject args{{"caller_cwd", proj}, {"path", "docs/specs/T-1-demo.md"}};

    // spec_lint falls back to ~/.claude/standards/spec-format.md's own
    // required-sections block when the fixture project carries no format
    // standard of its own (src/remotecontrol_docs.cpp
    // specLintRequiredSections, via QDir::homePath()) — real on this
    // machine (docs/standards/spec-format.md § 3 lists twelve). HOME is
    // pointed at the same empty temp dir as the config, which has no
    // `.claude` under it, so the fallback finds nothing and both sides
    // compare cleanly against the fixture's own (empty) required-section set.
    McpdSession stdio(QStringLiteral(ANTS_SOURCE_DIR), deadSocket(sock),
        QHash<QString, QString>{{QStringLiteral("XDG_CONFIG_HOME"), cfgStdio.path()},
                                {QStringLiteral("XDG_DATA_HOME"), dataStdio.path()},
                                {QStringLiteral("HOME"), cfgStdio.path()}});
    ASSERT_TRUE(stdio.started());
    const QJsonObject viaStdio = stdio.call(QStringLiteral("spec_lint"), args);
    ASSERT_FALSE(viaStdio.value(QStringLiteral("test_timeout")).toBool())
        << "reference stdio spec_lint call timed out: "
        << QJsonDocument(viaStdio).toJson().toStdString();
    ASSERT_TRUE(viaStdio.contains(QStringLiteral("findings")))
        << "reference stdio call (terse/offload off) should itself carry "
           "findings: " << QJsonDocument(viaStdio).toJson().toStdString();

    QProcessEnvironment envCall = QProcessEnvironment::systemEnvironment();
    envCall.insert(QStringLiteral("XDG_CONFIG_HOME"), cfgCall.path());
    envCall.insert(QStringLiteral("XDG_DATA_HOME"), dataCall.path());
    envCall.insert(QStringLiteral("HOME"), cfgCall.path());
    const CallOutcome viaCall = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("spec_lint"), compactJson(args)},
        proj, envCall);
    ASSERT_TRUE(viaCall.finished)
        << "ants-mcpd --call did not exit; stdout=" << viaCall.out.toStdString()
        << " stderr=" << viaCall.err.toStdString();
    EXPECT_EQ(viaCall.exitCode, 0) << "stderr=" << viaCall.err.toStdString();
    const QJsonObject fromCall = QJsonDocument::fromJson(viaCall.out.trimmed()).object();
    EXPECT_EQ(fromCall, viaStdio)
        << "--call stdout: " << viaCall.out.toStdString()
        << "\nexpected (stdio, unwrapped): "
        << QJsonDocument(viaStdio).toJson().toStdString();
}

// INV-2 — the exit code follows § 2.2's rules, applied in order: usage (2),
// then ok:false (1), then check_errors (1), then a missing findings key (2),
// then findings (3), else 0.
TEST(McpdCall, Inv2ExitCodesFollowSection2Point2) {
    QTemporaryDir tmp, home;
    ASSERT_TRUE(tmp.isValid() && home.isValid());
    // spec_lint falls back to ~/.claude's format standard when the fixture
    // carries none of its own (see Inv1's comment); HOME is redirected to an
    // empty temp dir so that fallback finds nothing.
    const QString cleanProj = writeSpecFixture(tmp, QStringLiteral("clean2"), /*clean=*/true);
    const QString dirtyProj = writeSpecFixture(tmp, QStringLiteral("dirty2"), /*clean=*/false);
    ASSERT_FALSE(cleanProj.isEmpty());
    ASSERT_FALSE(dirtyProj.isEmpty());

    const auto specArgs = [](const QString &cwd, const QString &path) {
        return compactJson(QJsonObject{{"caller_cwd", cwd}, {"path", path}});
    };

    struct Case {
        const char *label;
        QString verb;
        QString json;   // empty ⇒ no JSON argument at all (defaults to {})
        bool exitFlag;
        int expected;
    };
    const QList<Case> cases = {
        {"clean fixture, --exit-code", QStringLiteral("spec_lint"),
         specArgs(cleanProj, QStringLiteral("docs/specs/T-1-demo.md")), true, 0},
        {"one-finding fixture, --exit-code", QStringLiteral("spec_lint"),
         specArgs(dirtyProj, QStringLiteral("docs/specs/T-1-demo.md")), true, 3},
        {"one-finding fixture, no --exit-code", QStringLiteral("spec_lint"),
         specArgs(dirtyProj, QStringLiteral("docs/specs/T-1-demo.md")), false, 0},
        {"spec_lint path escapes the root (bad_path)", QStringLiteral("spec_lint"),
         specArgs(cleanProj, QStringLiteral("../outside.md")), true, 1},
        {"JSON argument is not an object ('[1]')", QStringLiteral("spec_lint"),
         QStringLiteral("[1]"), true, 2},
        {"terminal-scoped verb (tab_list)", QStringLiteral("tab_list"),
         QString(), true, 2},
        {"unregistered verb name", QStringLiteral("zzz_not_a_registered_verb_5506"),
         QString(), true, 2},
        {"tool_info descriptor has no findings key", QStringLiteral("tool_info"),
         QStringLiteral(R"({"name":"doc_lint"})"), true, 2},
    };

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), home.path());
    for (const Case &c : cases) {
        SCOPED_TRACE(c.label);
        QStringList args{QStringLiteral("--call"), c.verb};
        if (!c.json.isEmpty()) args << c.json;
        if (c.exitFlag) args << QStringLiteral("--exit-code");
        const CallOutcome r = runMcpdCall(
            args, cleanProj, env,
            /*closeStdin=*/true, /*timeoutMs=*/6000);
        ASSERT_TRUE(r.finished)
            << "ants-mcpd --call did not exit; stdout=" << r.out.toStdString()
            << " stderr=" << r.err.toStdString();
        EXPECT_EQ(r.exitCode, c.expected)
            << "stdout=" << r.out.toStdString() << " stderr=" << r.err.toStdString();
    }
}

// INV-3 — a result over the offload threshold is printed whole: --call turns
// offload off itself (§ 2.1 point 1), regardless of the child's own config.
//
// ANTS-5506 Q3 mutation finding — the original case called `tool_info`,
// which `mcp::isOffloadEligible` (src/mcpprojection.cpp) does not list, so
// it can never be offloaded regardless of whether --call disables offload:
// a mutant that left offload ON for --call still passed. `read_region` IS
// offload-eligible, so the fixture below is over the 4096-byte floor and
// the reply is the field offload would otherwise replace with a spill
// pointer.
TEST(McpdCall, Inv3OffloadIsForcedOff) {
    QTemporaryDir tmp, cfg, data;
    ASSERT_TRUE(tmp.isValid() && cfg.isValid() && data.isValid());
    ASSERT_TRUE(writeConfig(cfg.path(), QJsonObject{
        {"claude.mcp_offload_large_results", true},
        {"claude.mcp_offload_threshold_bytes", 4096}}));

    const QString proj = tmp.filePath(QStringLiteral("inv3"));
    ASSERT_TRUE(QDir().mkpath(proj));
    QFile f(proj + QStringLiteral("/fixture.txt"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    const QByteArray lineText = "line of fixture text for the offload floor\n";
    constexpr int kLines = 150;   // well over the 4096-byte floor; checked below
    QByteArray body;
    for (int i = 0; i < kLines; ++i) body += lineText;
    ASSERT_GT(body.size(), 4096) << "fixture assumption failed before any call";
    ASSERT_EQ(f.write(body), body.size());
    f.close();
    const QString root = QFileInfo(proj).canonicalFilePath();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), cfg.path());
    env.insert(QStringLiteral("XDG_DATA_HOME"), data.path());

    // offload:true is a reply-shaping arg --call must strip (§ 2.1); it is
    // included on purpose, the same way the earlier tool_info version did,
    // so a regression that lets it through is still exercised.
    const QString args = compactJson(QJsonObject{
        {"caller_cwd", root}, {"path", "fixture.txt"},
        {"start_line", 1}, {"end_line", 1000}, {"offload", true}});
    const CallOutcome r = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("read_region"), args}, root, env);
    ASSERT_TRUE(r.finished)
        << "ants-mcpd --call did not exit; stdout=" << r.out.toStdString()
        << " stderr=" << r.err.toStdString();
    ASSERT_EQ(r.exitCode, 0) << "stderr=" << r.err.toStdString();
    ASSERT_GT(r.out.size(), 4096)
        << "fixture assumption failed: read_region's reply must exceed the "
           "4096-byte offload floor on its own; stdout=" << r.out.toStdString();
    const QJsonObject respBody = QJsonDocument::fromJson(r.out.trimmed()).object();
    ASSERT_TRUE(respBody.value(QStringLiteral("ok")).toBool())
        << "stdout=" << r.out.toStdString();
    EXPECT_FALSE(respBody.contains(QStringLiteral("handle")))
        << "reply was offloaded despite --call disabling it: "
        << r.out.left(400).toStdString() << "...";
    EXPECT_FALSE(respBody.value(QStringLiteral("truncated")).toBool())
        << "reply was truncated, not the whole region: " << r.out.toStdString();
    EXPECT_EQ(respBody.value(QStringLiteral("returned")).toInt(), kLines)
        << "not the whole region (" << kLines << " lines written): "
        << r.out.toStdString();
}

// INV-4 — with --exit-code, a non-empty top-level check_errors gives 1, even
// though ok:true and findings is empty. Tests the mapping function alone.
TEST(McpdCall, Inv4CheckErrorsWithExitCodeIsFailed) {
    const QJsonObject envelope{
        {"ok", true},
        {"findings", QJsonArray{}},
        {"check_errors", QJsonArray{QStringLiteral("doc_integrity: walk aborted")}}};
    EXPECT_EQ(mcpd::exitCodeFor(envelope, /*envelopeParsed=*/true, /*exitCodeFlag=*/true),
              mcpd::CallFailed)
        << "a mapping that reads findings alone would call this CallClean or "
           "CallFindings instead of CallFailed";
}

// INV-4 (second envelope) — check_errors AND findings both non-empty still
// gives 1: could-not-check outranks found-a-problem. Why this exists: a
// mapping that tests findings before check_errors would return 3 (CallFindings).
TEST(McpdCall, Inv4CheckErrorsOutranksFindings) {
    const QJsonObject envelope{
        {"ok", true},
        {"findings", QJsonArray{QJsonObject{
            {"rule", QStringLiteral("spec_lint.invariant_no_test")},
            {"file", QStringLiteral("docs/specs/T-1-demo.md")},
            {"line", 12},
            {"message", QStringLiteral("invariant has no test")}}}},
        {"check_errors", QJsonArray{QStringLiteral("doc_integrity: walk aborted")}}};
    EXPECT_EQ(mcpd::exitCodeFor(envelope, /*envelopeParsed=*/true, /*exitCodeFlag=*/true),
              mcpd::CallFailed)
        << "check_errors and findings are both non-empty: expected CallFailed (1), "
           "could-not-check outranks found-a-problem; a mapping reading findings "
           "first would return CallFindings (3)";
}

// INV-5 — with a JSON argument, --call reads no stdin and exits after one
// reply. Stdin is left open for the whole wait; falling through to the
// stdio loop would wait on it forever.
TEST(McpdCall, Inv5DoesNotReadStdinWithAJsonArgument) {
    QTemporaryDir tmp, home;
    ASSERT_TRUE(tmp.isValid() && home.isValid());
    const QString proj = writeSpecFixture(tmp, QStringLiteral("inv5"), /*clean=*/true);
    ASSERT_FALSE(proj.isEmpty());
    const QString args = compactJson(
        QJsonObject{{"caller_cwd", proj}, {"path", "docs/specs/T-1-demo.md"}});
    // HOME redirected — see Inv1's comment on spec_lint's ~/.claude fallback.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), home.path());
    const CallOutcome r = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("spec_lint"), args}, proj,
        env, /*closeStdin=*/false, /*timeoutMs=*/10000);
    EXPECT_TRUE(r.finished)
        << "ants-mcpd --call did not exit within the bound with stdin left "
           "open; a JSON argument must make it answer once and quit without "
           "falling through to the stdio read loop. stdout="
        << r.out.toStdString() << " stderr=" << r.err.toStdString();
}

// INV-6 — a --call run leaves no file in TokenUsageEngine::peerSnapshotDir().
TEST(McpdCall, Inv6WritesNoUsageSnapshot) {
    QTemporaryDir cfg, data;
    ASSERT_TRUE(cfg.isValid() && data.isValid());
    const QString usageDir = data.path() + QStringLiteral("/ants-terminal/mcpd-usage");
    const auto snapshotFiles = [&] {
        QDir d(usageDir);
        return d.exists() ? d.entryList(QDir::Files) : QStringList();
    };
    ASSERT_TRUE(snapshotFiles().isEmpty()) << "test setup bug: usage dir pre-populated";

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), cfg.path());
    env.insert(QStringLiteral("XDG_DATA_HOME"), data.path());
    const CallOutcome r = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("tool_info"),
         QStringLiteral(R"({"catalog":true})")},
        QStringLiteral(ANTS_SOURCE_DIR), env);
    ASSERT_TRUE(r.finished)
        << "ants-mcpd --call did not exit; stdout=" << r.out.toStdString()
        << " stderr=" << r.err.toStdString();
    EXPECT_TRUE(snapshotFiles().isEmpty())
        << "ants-mcpd --call wrote a usage snapshot under " << usageDir.toStdString()
        << ": " << snapshotFiles().join(QStringLiteral(", ")).toStdString()
        << " — a --call run sets up no usage snapshot at all (§ 2.1 point 3)";
}

// INV-7 — with claude.mcp_enabled false, --call exits 1 with the master
// gate's refusal.
TEST(McpdCall, Inv7MasterGateRefusesWithExitOne) {
    QTemporaryDir cfg, data;
    ASSERT_TRUE(cfg.isValid() && data.isValid());
    ASSERT_TRUE(writeConfig(cfg.path(), QJsonObject{{"claude.mcp_enabled", false}}));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), cfg.path());
    env.insert(QStringLiteral("XDG_DATA_HOME"), data.path());
    const CallOutcome r = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("tool_info"),
         QStringLiteral(R"({"catalog":true})")},
        QStringLiteral(ANTS_SOURCE_DIR), env);
    ASSERT_TRUE(r.finished)
        << "ants-mcpd --call did not exit; stdout=" << r.out.toStdString()
        << " stderr=" << r.err.toStdString();
    EXPECT_EQ(r.exitCode, 1) << "stdout=" << r.out.toStdString();
    EXPECT_TRUE(r.out.contains("mcp_disabled"))
        << "stdout did not carry the master gate's refusal: " << r.out.toStdString();
}

// INV-8 — with no caller_cwd in the arguments, a Required verb runs against
// the process's working directory.
TEST(McpdCall, Inv8NoCallerCwdUsesProcessCwd) {
    QTemporaryDir tmp, home;
    ASSERT_TRUE(tmp.isValid() && home.isValid());
    const QString proj = writeSpecFixture(tmp, QStringLiteral("inv8"), /*clean=*/true);
    ASSERT_FALSE(proj.isEmpty());
    // HOME redirected — see Inv1's comment on spec_lint's ~/.claude fallback.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), home.path());
    const CallOutcome r = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("spec_lint"), QStringLiteral("{}")},
        proj, env);
    ASSERT_TRUE(r.finished)
        << "ants-mcpd --call did not exit; stdout=" << r.out.toStdString()
        << " stderr=" << r.err.toStdString();
    const QJsonObject body = QJsonDocument::fromJson(r.out.trimmed()).object();
    EXPECT_TRUE(body.value(QStringLiteral("ok")).toBool())
        << "stdout=" << r.out.toStdString() << " stderr=" << r.err.toStdString();
    bool sawFixtureSpec = false;
    for (const auto &v : body.value(QStringLiteral("checked_docs")).toArray())
        if (v.toString().contains(QStringLiteral("T-1-demo.md"))) sawFixtureSpec = true;
    EXPECT_TRUE(sawFixtureSpec)
        << "checked_docs did not include the fixture project's spec — "
           "caller_cwd was not synthesised from the process cwd: "
        << r.out.toStdString();
}

// INV-9 — compact, offload, fields and raw never reach the verb, and terse
// responses are off, so a clean reply's empty findings array survives.
TEST(McpdCall, Inv9ReplyShapingArgsAndTerseAreOverridden) {
    QTemporaryDir tmp, cfg, data;
    ASSERT_TRUE(tmp.isValid() && cfg.isValid() && data.isValid());
    ASSERT_TRUE(writeConfig(cfg.path(), QJsonObject{{"claude.mcp_terse_responses", true}}));
    const QString proj = writeDocLintCleanFixture(tmp, QStringLiteral("inv9"));
    ASSERT_FALSE(proj.isEmpty());

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), cfg.path());
    env.insert(QStringLiteral("XDG_DATA_HOME"), data.path());

    const QString args = compactJson(QJsonObject{
        {"caller_cwd", proj},
        {"compact", true},
        {"fields", QJsonArray{QStringLiteral("ok")}}});
    const CallOutcome r = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("doc_lint"), args,
         QStringLiteral("--exit-code")},
        proj, env);
    ASSERT_TRUE(r.finished)
        << "ants-mcpd --call did not exit; stdout=" << r.out.toStdString()
        << " stderr=" << r.err.toStdString();
    const QJsonObject body = QJsonDocument::fromJson(r.out.trimmed()).object();
    EXPECT_TRUE(body.contains(QStringLiteral("findings")))
        << "findings key missing — compact/fields reached the verb, or terse "
           "responses stayed on: " << r.out.toStdString();
    EXPECT_EQ(r.exitCode, 0)
        << "a clean doc_lint reply with findings present must exit 0: stdout="
        << r.out.toStdString();
}

// INV-9 (§ 2.1 point 2's "terse responses are off") on a verb terse
// compaction actually changes.
//
// ANTS-5506 Q3 mutation finding — Inv1 and Inv9 above both call spec_lint /
// doc_lint, and neither is in `mcp::kDispatchProjection`
// (src/mcpprojection.cpp: isDefaultCompactTool), so terseDefault() being on
// or off makes no difference to either reply's shape — a mutant that left
// terse ON for --call passed both untouched. `read_region` IS
// default-compact-eligible, and a clean (non-truncated) read's
// `truncated:false` is exactly the kind of field default compaction drops
// (mcpprojection.cpp isCompactDroppable: false booleans are dead weight,
// and `truncated` is not in the protected-key list). The stdio call first
// proves the fixture/verb pairing CAN fail — with terse genuinely on, the
// field really is dropped — before checking that --call keeps it despite
// its own child's config also having terse on.
TEST(McpdCall, Inv9TerseIsForcedOffForADefaultCompactVerb) {
    QTemporaryDir tmp, sock, cfgStdio, dataStdio, cfgCall, dataCall;
    ASSERT_TRUE(tmp.isValid() && sock.isValid() && cfgStdio.isValid() &&
                dataStdio.isValid() && cfgCall.isValid() && dataCall.isValid());

    const QString proj = tmp.filePath(QStringLiteral("inv9terse"));
    ASSERT_TRUE(QDir().mkpath(proj));
    QFile f(proj + QStringLiteral("/fixture.txt"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    const QByteArray body = "a few\nshort\nlines of fixture text\n";
    ASSERT_EQ(f.write(body), body.size());
    f.close();
    const QString root = QFileInfo(proj).canonicalFilePath();

    // Both sides' configs have terse ON — the stdio side is the control
    // (proving terse-on really does drop the field for this verb), and the
    // --call side is the opposite of what --call must force, so a mutant
    // that merely inherits the config would match the control instead of
    // the expected shape.
    ASSERT_TRUE(writeConfig(cfgStdio.path(),
        QJsonObject{{"claude.mcp_terse_responses", true}}));
    ASSERT_TRUE(writeConfig(cfgCall.path(),
        QJsonObject{{"claude.mcp_terse_responses", true}}));

    const QJsonObject args{{"caller_cwd", root}, {"path", "fixture.txt"},
                           {"start_line", 1}, {"end_line", 1000}};

    McpdSession stdio(QStringLiteral(ANTS_SOURCE_DIR), deadSocket(sock),
        QHash<QString, QString>{{QStringLiteral("XDG_CONFIG_HOME"), cfgStdio.path()},
                                {QStringLiteral("XDG_DATA_HOME"), dataStdio.path()}});
    ASSERT_TRUE(stdio.started());
    const QJsonObject viaStdio = stdio.call(QStringLiteral("read_region"), args);
    ASSERT_FALSE(viaStdio.value(QStringLiteral("test_timeout")).toBool())
        << QJsonDocument(viaStdio).toJson().toStdString();
    ASSERT_TRUE(viaStdio.value(QStringLiteral("ok")).toBool())
        << QJsonDocument(viaStdio).toJson().toStdString();
    ASSERT_FALSE(viaStdio.contains(QStringLiteral("truncated")))
        << "control check failed: over stdio with terse genuinely on, "
           "read_region's false `truncated` should have been compacted "
           "away by default — this fixture/verb pairing cannot distinguish "
           "terse on from terse off: "
        << QJsonDocument(viaStdio).toJson().toStdString();

    QProcessEnvironment envCall = QProcessEnvironment::systemEnvironment();
    envCall.insert(QStringLiteral("XDG_CONFIG_HOME"), cfgCall.path());
    envCall.insert(QStringLiteral("XDG_DATA_HOME"), dataCall.path());
    const CallOutcome viaCall = runMcpdCall(
        {QStringLiteral("--call"), QStringLiteral("read_region"), compactJson(args)},
        root, envCall);
    ASSERT_TRUE(viaCall.finished)
        << "ants-mcpd --call did not exit; stdout=" << viaCall.out.toStdString()
        << " stderr=" << viaCall.err.toStdString();
    ASSERT_EQ(viaCall.exitCode, 0) << "stderr=" << viaCall.err.toStdString();
    const QJsonObject fromCall = QJsonDocument::fromJson(viaCall.out.trimmed()).object();
    ASSERT_TRUE(fromCall.value(QStringLiteral("ok")).toBool())
        << "stdout=" << viaCall.out.toStdString();
    EXPECT_TRUE(fromCall.contains(QStringLiteral("truncated")))
        << "read_region's false `truncated` was dropped from --call's "
           "stdout — --call's own child config has terse on, and this "
           "reply only keeps the field if terse was forced off regardless: "
        << viaCall.out.toStdString();
}

// § 2.1 — no JSON argument at all parses to an empty-object args, not a read
// from stdin.
TEST(McpdCall, ParseCallRequestNoJsonArgumentDefaultsToEmptyObject) {
    const mcpd::CallRequest req = mcpd::parseCallRequest(
        {QStringLiteral("--call"), QStringLiteral("spec_lint")});
    EXPECT_TRUE(req.isCall);
    EXPECT_EQ(req.verb, QStringLiteral("spec_lint"));
    EXPECT_EQ(req.args, QJsonObject{});
    EXPECT_FALSE(req.readStdin);
    EXPECT_TRUE(req.usageError.isEmpty()) << req.usageError.toStdString();
}

// § 2.1 — only an explicit "-" asks for stdin.
TEST(McpdCall, ParseCallRequestDashReadsStdin) {
    const mcpd::CallRequest req = mcpd::parseCallRequest(
        {QStringLiteral("--call"), QStringLiteral("spec_lint"), QStringLiteral("-")});
    EXPECT_TRUE(req.isCall);
    EXPECT_EQ(req.verb, QStringLiteral("spec_lint"));
    EXPECT_TRUE(req.readStdin) << "an explicit \"-\" JSON argument must set readStdin";
}

// § 2.1 point 4 — a wrapped tool result comes back as the bare JSON.
TEST(McpdCall, UnwrapToolTextStripsTheWrapWhenPresent) {
    const QString wrapped = QStringLiteral(
        "<ants_mcp_data tool=\"spec_lint\">{\"ok\":true}</ants_mcp_data>");
    EXPECT_EQ(mcpd::unwrapToolText(wrapped), QStringLiteral("{\"ok\":true}"))
        << "wrapped: " << wrapped.toStdString();
}

// § 2.1 point 4 — control-plane text (never wrapped) comes back unchanged.
TEST(McpdCall, UnwrapToolTextLeavesControlPlaneTextUnchanged) {
    const QString unwrapped = QStringLiteral("{\"ok\":true,\"tool_count\":3}");
    EXPECT_EQ(mcpd::unwrapToolText(unwrapped), unwrapped);
}
