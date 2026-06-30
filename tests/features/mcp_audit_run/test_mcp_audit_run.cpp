// ANTS-1351 v1 — source-scrape regression test for audit_run.
// v1 covers the infrastructure invariants; behavioural tests
// (cap timing, env scrub, real tool spawning) land in v2.

#include "../../_support/expect.h"
#include "auditrunner.h"
#include "auditengine.h"  // ANTS-2182 — resolveCompileCommands

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QDir>

#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — aggregate cap constant.
TEST(mcp_audit_run, Inv1AggregateCapConstant) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "kAggregateCapMs            = 240'000"),
           "INV-1: aggregate cap = 240 s");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — caller_cwd canonicalisation + isDir check.
TEST(mcp_audit_run, Inv2PathValidationCanonical) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "callerFi.canonicalFilePath()"),
           "INV-2: canonicalFilePath called on caller_cwd");
    expect(contains(cpp, "QFileInfo(canonProject).isDir()"),
           "INV-2: isDir check on canonical path");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — Required contract.
TEST(mcp_audit_run, Inv3RequiredContract) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "QStringLiteral(\"audit_run\"))          return C::Required"),
           "INV-3: audit_run classified Required");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — SIGTERM-then-SIGKILL pattern.
TEST(mcp_audit_run, Inv5TerminateThenKill) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "proc->terminate()") &&
           contains(cpp, "proc->kill()"),
           "INV-5: terminate then kill on cap exceed");
    expect(contains(cpp, "kKillGraceMs"),
           "INV-5: 2 s kill grace constant");
    EXPECT_EQ(0, expect_failures());
}

// INV-9 — inline in-flight gate.
TEST(mcp_audit_run, Inv9InFlightGateInline) {
    expect_reset();
    const std::string h = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    expect(contains(h, "m_verbInFlight"),
           "INV-9: m_verbInFlight QHash member declared");
    expect(contains(h, "verbInFlightTryAcquire"),
           "INV-9: tryAcquire helper declared");
    expect(contains(h, "verbInFlightRelease"),
           "INV-9: release helper declared");
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "verbInFlightTryAcquire("),
           "INV-9: gate acquired in audit_run dispatch");
    expect(contains(mw, "verbInFlightRelease("),
           "INV-9: gate released after run");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — env allowlist/blocklist + tool resolve cache.
TEST(mcp_audit_run, Inv10EnvScrubToolResolve) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "kEnvAllowlist"),
           "INV-10: env allowlist declared");
    expect(contains(cpp, "kEnvBlocklist"),
           "INV-10: env blocklist declared");
    expect(contains(cpp, "SSH_AUTH_SOCK"),
           "INV-10: SSH_AUTH_SOCK on blocklist");
    expect(contains(cpp, "PATH"),
           "INV-10: PATH on allowlist");
    expect(contains(cpp, "AWS_"),
           "INV-10: AWS_* prefix block");
    expect(contains(cpp, "QStandardPaths::findExecutable"),
           "INV-10: absolute-path tool resolution");
    expect(contains(cpp, "kToolResolveCacheTtlMs"),
           "INV-10: 60 s TTL cache constant");
    EXPECT_EQ(0, expect_failures());
}

// INV-13 — sample-message 256 B cap (behavioural).
TEST(mcp_audit_run, Inv13MessageCap) {
    expect_reset();
    QString shortMsg = QStringLiteral("hello");
    expect(AuditRunner::internal::capMessage(shortMsg) == shortMsg,
           "INV-13: short message passes through unchanged");
    QString longMsg(1024, QLatin1Char('x'));
    const QString capped = AuditRunner::internal::capMessage(longMsg);
    expect(capped.toUtf8().size() <= 256,
           "INV-13: long message capped to ≤ 256 B");
    expect(capped.endsWith(QStringLiteral("…")),
           "INV-13: capped message ends with ellipsis");
    EXPECT_EQ(0, expect_failures());
}

// INV-15 — scope tag sanitisation (source anchor).
TEST(mcp_audit_run, Inv15ScopeTagRegex) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "^[A-Za-z0-9._/+-]{1,128}$"),
           "INV-15: tag sanitisation regex present");
    expect(contains(cpp, "startsWith(QLatin1Char('-'))"),
           "INV-15: leading-'-' reject");
    EXPECT_EQ(0, expect_failures());
}

// INV-16 — range check refusals.
TEST(mcp_audit_run, Inv16CapRanges) {
    expect_reset();
    // Hermetic projectRoot — never use real /tmp here, as
    // AuditRunner::runAudit may now (post AR-1) walk for src/ to
    // auto-detect a compile-commands path. A QTemporaryDir keeps the
    // refusal-path-only contract intact.
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        expect(false, "INV-16: QTemporaryDir setup");
        return;
    }
    const QString root = tmp.path();

    AuditRunner::RunRequest bad1;
    bad1.projectRoot = root;
    bad1.capPerToolSeconds = 4;  // < 5
    AuditRunner::RunResult r1 = AuditRunner::runAudit(bad1);
    expect(!r1.ok && r1.code == QStringLiteral("bad_args"),
           "INV-16: cap < 5 → bad_args");

    AuditRunner::RunRequest bad2;
    bad2.projectRoot = root;
    bad2.capPerToolSeconds = 61;  // > 60
    AuditRunner::RunResult r2 = AuditRunner::runAudit(bad2);
    expect(!r2.ok && r2.code == QStringLiteral("bad_args"),
           "INV-16: cap > 60 → bad_args");

    AuditRunner::RunRequest bad3;
    bad3.projectRoot = root;
    bad3.topFindingsCount = 101;
    AuditRunner::RunResult r3 = AuditRunner::runAudit(bad3);
    expect(!r3.ok && r3.code == QStringLiteral("bad_args"),
           "INV-16: top_findings_count > 100 → bad_args");
    EXPECT_EQ(0, expect_failures());
}

// Schema — descriptor block registered with audit_run name.
TEST(mcp_audit_run, SchemaDescriptorRegistered) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "t[\"name\"] = \"audit_run\""),
           "schema: audit_run descriptor present");
    expect(contains(cpp, "ANTS-1351 — audit_run"),
           "schema: ANTS-1351 anchor in descriptor block");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1456 AR-1/AR-2 — toolArgv auto-detects src/ for flat-layout
// projects so cppcheck/bandit don't silently fail on the missing
// hardcoded path.
TEST(mcp_audit_run, Ants1456SrcAutoDetect) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "hasSrcDir"),
           "AR-1: toolArgv must branch on hasSrcDir");
    expect(contains(cpp,
               "QFileInfo(\n        projectRoot + QLatin1String(\"/src\"))"),
           "AR-1: src/ existence is probed via QFileInfo");
    expect(contains(cpp, "const QString srcRoot ="),
           "AR-1: srcRoot constant captures the chosen scan path");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1456 AR-3/AR-4 — loadProjectAuditConfig + per-tool override.
TEST(mcp_audit_run, Ants1456ProjectAuditConfig) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "loadProjectAuditConfig"),
           "AR-3: loadProjectAuditConfig helper present");
    expect(contains(cpp, ".audit-config.json"),
           "AR-3: canonical .audit-config.json path probed");
    expect(contains(cpp,
               "docs/private/audit/audit-config.json"),
           "AR-3: RetroArch-style fallback path probed");
    expect(contains(cpp, "projectConfig.contains(tool)"),
           "AR-4: toolArgv consults projectConfig before default "
           "argv");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1456 AR-5 — SARIF emit exposes config-warning signal.
TEST(mcp_audit_run, Ants1456SarifConfigWarningProperty) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "executionSuccessfulWithConfigWarnings"),
           "AR-5: SARIF invocation properties carry "
           "executionSuccessfulWithConfigWarnings");
    expect(contains(cpp, "tr.rawCount == 0"),
           "AR-5: the property is set on raw-count-zero clean runs");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-1456 AR-6 — audit_run descriptor surfaces scope:"auto" semantics.
TEST(mcp_audit_run, Ants1456ScopeDescriptorClarified) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    // Locate the audit_run descriptor block by anchoring on its name.
    const auto pos = ci.find("t[\"name\"] = \"audit_run\"");
    ASSERT_NE(pos, std::string::npos);
    // Bound the probe to the audit_run descriptor block (anchor → its
    // tools.append) rather than a fixed char window: ANTS-1870 grew the
    // scope description past the old 5000-char slice, pushing the "full"
    // guidance out of range. Block-bounding is robust to future growth.
    const auto blockEnd = ci.find("tools.append(t);", pos);
    ASSERT_NE(blockEnd, std::string::npos);
    const std::string region = ci.substr(pos, blockEnd - pos);
    expect(contains(region, "deterministic full sweep"),
           "AR-6: scope description recommends an explicit "
           "since-tag override for deterministic full sweeps");
    expect(contains(region, "ANTS-1456"),
           "AR-6: scope description carries the ANTS-1456 anchor");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2188 INV-17 — raw tool output is secret-scrubbed at the single
// capture point (the `finish` lambda) before it reaches either sink:
// rawByTool (→ SARIF notification text on disk) or parseToolOutput
// (→ samples / top_findings in the MCP envelope). trivy's
// `--scanners secret` surfaces literal secret values; without this scrub
// they leak verbatim to the .audit_cache SARIF and back to the LLM
// (OWASP LLM06). gitleaks already runs --redact; trivy did not.
TEST(mcp_audit_run, Ants2188RawOutputSecretScrubbed) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "#include \"secretredact.h\""),
           "INV-17: auditrunner.cpp includes secretredact.h");
    // Bound the probe to the `finish` lambda body (anchor → its
    // parseToolOutput call) so the wiring assertions can't be satisfied
    // by an unrelated scrub elsewhere in the file.
    const auto pos = cpp.find("auto finish = [&](const QString &tool");
    ASSERT_NE(pos, std::string::npos);
    const auto blockEnd = cpp.find("r.byTool[tool]", pos);
    ASSERT_NE(blockEnd, std::string::npos);
    const std::string body = cpp.substr(pos, blockEnd - pos);
    expect(contains(body, "SecretRedact::scrub(rawOutput)"),
           "INV-17: finish() scrubs rawOutput through SecretRedact::scrub");
    // The scrubbed value — not the raw — must feed BOTH sinks.
    expect(contains(body, "rawByTool[tool] = scrubbed"),
           "INV-17: the scrubbed value feeds rawByTool (SARIF sink), "
           "not the raw output");
    expect(contains(body, "parseToolOutput(tool, scrubbed"),
           "INV-17: the scrubbed value feeds parseToolOutput (findings/"
           "samples sink), not the raw output");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2185 INV-18 — scoped positionals are guarded against argv
// option-injection: no tool branch may append the raw scopedPaths; every
// scoped path goes through the ./-guard first. A file named `-rf.cpp` in
// a hostile-clone tree must reach the child tool as a path, not a flag.
TEST(mcp_audit_run, Ants2185ToolArgvAppliesFlagGuard) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(!contains(cpp, "args += scopedPaths"),
           "INV-18: tool branches must append the guarded `scoped` list, "
           "not the raw scopedPaths (argv option-injection)");
    expect(contains(cpp, "flagSafeScopedPathImpl"),
           "INV-18: toolArgv normalises scoped positionals via the "
           "flag-safe guard");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2185 INV-18 — behavioural: the guard ./-prefixes dash-leading
// relative paths and leaves everything else byte-identical.
TEST(mcp_audit_run, Ants2185FlagSafeScopedPathTransform) {
    expect_reset();
    using AuditRunner::internal::flagSafeScopedPath;
    expect(flagSafeScopedPath(QStringLiteral("-x.cpp"))
               == QStringLiteral("./-x.cpp"),
           "INV-18: a dash-leading relative path is ./-prefixed");
    expect(flagSafeScopedPath(QStringLiteral("--config=evil"))
               == QStringLiteral("./--config=evil"),
           "INV-18: a long-option-shaped name is ./-prefixed");
    expect(flagSafeScopedPath(QStringLiteral("src/a.py"))
               == QStringLiteral("src/a.py"),
           "INV-18: an ordinary relative path passes through unchanged");
    expect(flagSafeScopedPath(QStringLiteral("/abs/-y.c"))
               == QStringLiteral("/abs/-y.c"),
           "INV-18: an absolute path passes through unchanged "
           "(never flag-parsed)");
    EXPECT_EQ(0, expect_failures());
}

// Dispatch — provider lambda registered in mainwindow.
TEST(mcp_audit_run, DispatchProviderRegistered) {
    expect_reset();
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "registerToolProvider(\"audit_run\""),
           "dispatch: audit_run provider registered");
    expect(contains(mw, "AuditRunner::runAudit(req)"),
           "dispatch: provider calls AuditRunner::runAudit");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2182 INV-19 — behavioural: resolveCompileCommands probes the
// canonical build-dir variants and returns the first existing
// compile_commands.json (and prefers `build/` when several exist).
TEST(mcp_audit_run, Ants2182ResolveCompileCommandsProbesBuildDirs) {
    expect_reset();
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    const QString base = root.path();

    // No build dir yet → empty.
    expect(AuditEngine::resolveCompileCommands(base).isEmpty(),
           "INV-19: no compile DB anywhere → empty string");

    // DB only in build-fast/ (a non-`build/` tree) → found there. This is
    // the exact ANTS-2182 case where clazy used to hardcode build/ and get 0.
    ASSERT_TRUE(QDir(base).mkpath(QStringLiteral("build-fast")));
    const QString fastDb =
        base + QStringLiteral("/build-fast/compile_commands.json");
    { QFile f(fastDb); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("[]"); }
    expect(AuditEngine::resolveCompileCommands(base) == fastDb,
           "INV-19: DB in build-fast/ is resolved when build/ is absent");

    // Now add build/ too → build/ wins (it is first in the probe order).
    ASSERT_TRUE(QDir(base).mkpath(QStringLiteral("build")));
    const QString buildDb =
        base + QStringLiteral("/build/compile_commands.json");
    { QFile f(buildDb); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("[]"); }
    expect(AuditEngine::resolveCompileCommands(base) == buildDb,
           "INV-19: build/ takes precedence over build-fast/");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2182 INV-19 — source-scrape: the C/C++ tool branches in toolArgv
// resolve the compile DB through the shared helper instead of a bare
// hardcoded `build/compile_commands.json`, and cppcheck drives off
// --project when a DB is present.
TEST(mcp_audit_run, Ants2182ToolArgvUsesCompileDb) {
    expect_reset();
    const std::string cpp = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "AuditEngine::resolveCompileCommands(projectRoot)"),
           "INV-19: toolArgv resolves the compile DB via the shared helper");
    expect(contains(cpp, "--project="),
           "INV-19: cppcheck drives off --project=<db> when present");
    expect(contains(cpp, "--suppress=missingIncludeSystem"),
           "INV-19: cppcheck suppresses the missingIncludeSystem flood "
           "(no-DB fallback)");
    EXPECT_EQ(0, expect_failures());
}

// ANTS-2183 INV-20 — the audit_run descriptor warns that a full sweep can
// trip the client's transport timer and to read results via
// last_audit_summary (the run still completes server-side).
TEST(mcp_audit_run, Ants2183TransportCapDocumented) {
    expect_reset();
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const auto pos = ci.find("t[\"name\"] = \"audit_run\"");
    ASSERT_NE(pos, std::string::npos);
    const auto blockEnd = ci.find("tools.append(t);", pos);
    ASSERT_NE(blockEnd, std::string::npos);
    const std::string region = ci.substr(pos, blockEnd - pos);
    expect(contains(region, "last_audit_summary"),
           "INV-20: descriptor points the caller at last_audit_summary "
           "for long sweeps");
    expect(contains(region, "ANTS-2183"),
           "INV-20: descriptor carries the ANTS-2183 anchor");
    EXPECT_EQ(0, expect_failures());
}
