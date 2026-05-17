// ANTS-1351 v1 — source-scrape regression test for audit_run.
// v1 covers the infrastructure invariants; behavioural tests
// (cap timing, env scrub, real tool spawning) land in v2.

#include "../../_support/expect.h"
#include "auditrunner.h"

#include <gtest/gtest.h>

#include <QHash>
#include <QJsonArray>
#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "setup-fail: cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// INV-1 — aggregate cap constant.
TEST(mcp_audit_run, Inv1AggregateCapConstant) {
    expect_reset();
    const std::string cpp = slurp(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "kAggregateCapMs            = 240'000"),
           "INV-1: aggregate cap = 240 s");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — caller_cwd canonicalisation + isDir check.
TEST(mcp_audit_run, Inv2PathValidationCanonical) {
    expect_reset();
    const std::string cpp = slurp(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "callerFi.canonicalFilePath()"),
           "INV-2: canonicalFilePath called on caller_cwd");
    expect(contains(cpp, "QFileInfo(canonProject).isDir()"),
           "INV-2: isDir check on canonical path");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — Required contract.
TEST(mcp_audit_run, Inv3RequiredContract) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "QStringLiteral(\"audit_run\"))          return C::Required"),
           "INV-3: audit_run classified Required");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — SIGTERM-then-SIGKILL pattern.
TEST(mcp_audit_run, Inv5TerminateThenKill) {
    expect_reset();
    const std::string cpp = slurp(SRC_AUDITRUNNER_CPP_PATH);
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
    const std::string h = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    expect(contains(h, "m_verbInFlight"),
           "INV-9: m_verbInFlight QHash member declared");
    expect(contains(h, "verbInFlightTryAcquire"),
           "INV-9: tryAcquire helper declared");
    expect(contains(h, "verbInFlightRelease"),
           "INV-9: release helper declared");
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "verbInFlightTryAcquire("),
           "INV-9: gate acquired in audit_run dispatch");
    expect(contains(mw, "verbInFlightRelease("),
           "INV-9: gate released after run");
    EXPECT_EQ(0, expect_failures());
}

// INV-10 — env allowlist/blocklist + tool resolve cache.
TEST(mcp_audit_run, Inv10EnvScrubToolResolve) {
    expect_reset();
    const std::string cpp = slurp(SRC_AUDITRUNNER_CPP_PATH);
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
    const std::string cpp = slurp(SRC_AUDITRUNNER_CPP_PATH);
    expect(contains(cpp, "^[A-Za-z0-9._/+-]{1,128}$"),
           "INV-15: tag sanitisation regex present");
    expect(contains(cpp, "startsWith(QLatin1Char('-'))"),
           "INV-15: leading-'-' reject");
    EXPECT_EQ(0, expect_failures());
}

// INV-16 — range check refusals.
TEST(mcp_audit_run, Inv16CapRanges) {
    expect_reset();
    AuditRunner::RunRequest bad1;
    bad1.projectRoot = QStringLiteral("/tmp");
    bad1.capPerToolSeconds = 4;  // < 5
    AuditRunner::RunResult r1 = AuditRunner::runAudit(bad1);
    expect(!r1.ok && r1.code == QStringLiteral("bad_args"),
           "INV-16: cap < 5 → bad_args");

    AuditRunner::RunRequest bad2;
    bad2.projectRoot = QStringLiteral("/tmp");
    bad2.capPerToolSeconds = 61;  // > 60
    AuditRunner::RunResult r2 = AuditRunner::runAudit(bad2);
    expect(!r2.ok && r2.code == QStringLiteral("bad_args"),
           "INV-16: cap > 60 → bad_args");

    AuditRunner::RunRequest bad3;
    bad3.projectRoot = QStringLiteral("/tmp");
    bad3.topFindingsCount = 101;
    AuditRunner::RunResult r3 = AuditRunner::runAudit(bad3);
    expect(!r3.ok && r3.code == QStringLiteral("bad_args"),
           "INV-16: top_findings_count > 100 → bad_args");
    EXPECT_EQ(0, expect_failures());
}

// Schema — descriptor block registered with audit_run name.
TEST(mcp_audit_run, SchemaDescriptorRegistered) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    expect(contains(cpp, "t[\"name\"] = \"audit_run\""),
           "schema: audit_run descriptor present");
    expect(contains(cpp, "ANTS-1351 — audit_run"),
           "schema: ANTS-1351 anchor in descriptor block");
    EXPECT_EQ(0, expect_failures());
}

// Dispatch — provider lambda registered in mainwindow.
TEST(mcp_audit_run, DispatchProviderRegistered) {
    expect_reset();
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    expect(contains(mw, "registerToolProvider(\"audit_run\""),
           "dispatch: audit_run provider registered");
    expect(contains(mw, "AuditRunner::runAudit(req)"),
           "dispatch: provider calls AuditRunner::runAudit");
    EXPECT_EQ(0, expect_failures());
}
