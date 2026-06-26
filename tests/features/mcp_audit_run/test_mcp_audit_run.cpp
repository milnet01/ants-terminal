// ANTS-1351 v1 — source-scrape regression test for audit_run.
// v1 covers the infrastructure invariants; behavioural tests
// (cap timing, env scrub, real tool spawning) land in v2.

#include "../../_support/expect.h"
#include "auditrunner.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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
