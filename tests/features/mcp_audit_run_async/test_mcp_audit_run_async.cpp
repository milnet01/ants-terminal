// Feature-conformance test for ANTS-3396 — opt-in async audit_run +
// audit_poll. See tests/features/mcp_audit_run_async/spec.md.
//
// Two layers: (1) source-grep that the wiring is present in mainwindow /
// claudeintegration; (2) behavioural coverage of the bounded job registry
// on a live ClaudeIntegration instance (the registry methods are pure
// QHash/QMutex, so no event loop is needed).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <string>
#include "claudeintegration.h"

#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#  error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

namespace {

std::string mainwindowSrc() {
    return ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
}
std::string ciSrc() {
    return ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
}
bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
// Whitespace-insensitive containment — robust against column-alignment
// spacing in the source (e.g. the `return C::Required;` table).
std::string squash(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') out.push_back(c);
    return out;
}
bool hasWs(const std::string &hay, const char *needle) {
    return squash(hay).find(squash(needle)) != std::string::npos;
}

// A terminal "done" AuditJob with a cache path + counts.
ClaudeIntegration::AuditJob doneJob(const QString &cache, int raw, int act) {
    ClaudeIntegration::AuditJob j;
    j.status          = QStringLiteral("done");
    j.cachePath       = cache;
    j.totalRaw        = raw;
    j.totalActionable = act;
    return j;
}

}  // namespace

// ---- INV-1 — synchronous path unchanged, async branch is opt-in ----------

TEST(McpAuditRunAsync, Inv1SyncPathUnchanged) {
    const std::string mw = mainwindowSrc();
    // The synchronous join is still present verbatim.
    EXPECT_TRUE(has(mw, "worker->start();")) << "sync start() must remain";
    EXPECT_TRUE(has(mw, "worker->wait();"))
        << "sync join (wait()) must remain for the default path";
    // The async branch is gated on the per-call arg (default false).
    EXPECT_TRUE(has(mw, "QStringLiteral(\"async\")).toBool()"))
        << "async branch must be guarded by args(\"async\").toBool()";
}

// ---- INV-2 — async branch returns a handle without joining ---------------

TEST(McpAuditRunAsync, Inv2AsyncBranchNoJoin) {
    const std::string mw = mainwindowSrc();
    EXPECT_TRUE(has(mw, "auditJobRegister(canon, startedMs)"))
        << "async branch registers a job keyed by the canonical root";
    EXPECT_TRUE(hasWs(mw, "env[\"async\"] = true;"))
        << "async envelope carries async:true";
    EXPECT_TRUE(hasWs(mw, "env[\"poll_with\"] = QStringLiteral(\"audit_poll\")"))
        << "async envelope points the caller at audit_poll";
    EXPECT_TRUE(hasWs(mw, "env[\"status\"] = QStringLiteral(\"running\")"))
        << "async envelope reports status:running";
}

// ---- INV-3 — registry running → done / error, with fallbacks -------------

TEST(McpAuditRunAsync, Inv3RegistryRunningThenDone) {
    ClaudeIntegration ci;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const QString rootA = QStringLiteral("/root/a");
    const QString jobId = ci.auditJobRegister(rootA, now);
    ASSERT_EQ(jobId, QStringLiteral("audit-1"));

    // Before completion → running (with a liveness elapsed_ms).
    QJsonObject running = ci.auditJobPollEnvelope(jobId, rootA);
    EXPECT_TRUE(running.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(running.value(QStringLiteral("status")).toString(),
              QStringLiteral("running"));
    EXPECT_TRUE(running.contains(QStringLiteral("elapsed_ms")));

    // Flip to done → cache_path + compact counts + read_full_with.
    ci.auditJobComplete(jobId, doneJob(QStringLiteral("/root/a/.audit_cache/x.sarif"),
                                       142, 9));
    QJsonObject done = ci.auditJobPollEnvelope(jobId, rootA);
    EXPECT_EQ(done.value(QStringLiteral("status")).toString(),
              QStringLiteral("done"));
    EXPECT_EQ(done.value(QStringLiteral("cache_path")).toString(),
              QStringLiteral("/root/a/.audit_cache/x.sarif"));
    EXPECT_EQ(done.value(QStringLiteral("total_raw")).toInt(), 142);
    EXPECT_EQ(done.value(QStringLiteral("total_actionable")).toInt(), 9);
    EXPECT_EQ(done.value(QStringLiteral("read_full_with")).toString(),
              QStringLiteral("last_audit_summary"));

    // Idempotent re-poll of a done entry.
    EXPECT_EQ(ci.auditJobPollEnvelope(jobId, rootA).value(QStringLiteral("status"))
                  .toString(), QStringLiteral("done"));

    // Cross-root scoping — the same job_id from a DIFFERENT project root
    // reads as `expired`, never leaking the cache_path.
    QJsonObject crossRoot =
        ci.auditJobPollEnvelope(jobId, QStringLiteral("/root/other"));
    EXPECT_EQ(crossRoot.value(QStringLiteral("status")).toString(),
              QStringLiteral("expired"));
    EXPECT_FALSE(crossRoot.contains(QStringLiteral("cache_path")));

    // Error path — status:"error", carries the run's code.
    const QString rootB = QStringLiteral("/root/b");
    const QString errId = ci.auditJobRegister(rootB, now);
    ClaudeIntegration::AuditJob err;
    err.status = QStringLiteral("error");
    err.code   = QStringLiteral("tool_spawn_failed");
    err.error  = QStringLiteral("cppcheck not found");
    ci.auditJobComplete(errId, err);
    QJsonObject errEnv = ci.auditJobPollEnvelope(errId, rootB);
    EXPECT_TRUE(errEnv.value(QStringLiteral("ok")).toBool())
        << "poll itself succeeds; the job outcome lives in status";
    EXPECT_EQ(errEnv.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(errEnv.value(QStringLiteral("code")).toString(),
              QStringLiteral("tool_spawn_failed"));
}

// ---- INV-4 — bounded registry, eviction, expiry, saturation --------------

TEST(McpAuditRunAsync, Inv4RegistryBoundAndExpiry) {
    ClaudeIntegration ci;
    const qint64 base = QDateTime::currentMSecsSinceEpoch();

    // Register 16 jobs (recent, increasing start times) and flip each to
    // a terminal state so they are all evictable.
    QString firstId;
    QString firstRoot;
    for (int i = 0; i < 16; ++i) {
        const QString root = QStringLiteral("/root/%1").arg(i);
        const QString id = ci.auditJobRegister(root, base + i);
        ASSERT_FALSE(id.isEmpty());
        if (i == 0) { firstId = id; firstRoot = root; }
        ci.auditJobComplete(id, doneJob(QStringLiteral("/c/%1.sarif").arg(i), 0, 0));
    }
    // A 17th registration is at the cap → evict the oldest terminal (job 0).
    const QString id17 = ci.auditJobRegister(QStringLiteral("/root/x"),
                                             base + 100);
    ASSERT_FALSE(id17.isEmpty()) << "eviction must make room for a new job";

    // The evicted id now polls `expired` (ok:true, no code, recovery hint).
    QJsonObject expired = ci.auditJobPollEnvelope(firstId, firstRoot);
    EXPECT_TRUE(expired.value(QStringLiteral("ok")).toBool());
    EXPECT_EQ(expired.value(QStringLiteral("status")).toString(),
              QStringLiteral("expired"));
    EXPECT_FALSE(expired.contains(QStringLiteral("code")));
    EXPECT_TRUE(expired.value(QStringLiteral("hint")).toString()
                    .contains(QStringLiteral("last_audit_summary")));

    // A never-minted id is also `expired`.
    EXPECT_EQ(ci.auditJobPollEnvelope(QStringLiteral("audit-9999"),
                                      QStringLiteral("/root/0"))
                  .value(QStringLiteral("status")).toString(),
              QStringLiteral("expired"));
}

TEST(McpAuditRunAsync, Inv4AllRunningSaturationRefuses) {
    ClaudeIntegration ci;
    const qint64 base = QDateTime::currentMSecsSinceEpoch();
    // 16 RUNNING jobs (never completed) — none evictable.
    for (int i = 0; i < 16; ++i) {
        const QString id = ci.auditJobRegister(
            QStringLiteral("/root/%1").arg(i), base + i);
        ASSERT_FALSE(id.isEmpty());
    }
    // The 17th finds all entries running → empty (caller emits too_many_jobs).
    EXPECT_TRUE(ci.auditJobRegister(QStringLiteral("/root/x"), base + 100)
                    .isEmpty())
        << "a full registry of running jobs must refuse a new registration";
}

// ---- INV-5 — queued completion, guard dismissal, mutex -------------------

TEST(McpAuditRunAsync, Inv5QueuedCompletionAndGuardDismiss) {
    const std::string mw = mainwindowSrc();
    EXPECT_TRUE(has(mw, "Qt::QueuedConnection"))
        << "completion must be a queued post to the main thread";
    EXPECT_TRUE(has(mw, "inFlightGuard.dismiss()"))
        << "async branch must hand the in-flight release to the completion slot";
    EXPECT_TRUE(has(mw, "ci->verbInFlightRelease("))
        << "completion slot releases the in-flight slot";
    const std::string ci = ciSrc();
    EXPECT_TRUE(has(ci, "QMutexLocker lk(&m_auditJobsMutex)"))
        << "registry is mutated only under its mutex";
}

// ---- INV-8 — refusal wiring ----------------------------------------------

TEST(McpAuditRunAsync, Inv8RefusalWiring) {
    const std::string ci = ciSrc();
    EXPECT_TRUE(hasWs(ci,
        "QStringLiteral(\"audit_poll\")) return C::Required"))
        << "audit_poll must be Required in callerCwdContractFor";
    const std::string mw = mainwindowSrc();
    EXPECT_TRUE(has(mw, "\"audit_poll\""))
        << "audit_poll provider must be registered";
    EXPECT_TRUE(has(mw, "QStringLiteral(\"bad_args\")"))
        << "audit_poll must refuse a missing job_id with bad_args";
    EXPECT_TRUE(has(mw, "QStringLiteral(\"too_many_jobs\")"))
        << "async saturation must refuse with too_many_jobs";
}

// ---- INV-9 — no new config/Settings key ----------------------------------

TEST(McpAuditRunAsync, Inv9NoNewConfigKey) {
    const std::string mw = mainwindowSrc();
    // The async / audit_poll paths introduce no new claude.* config read.
    EXPECT_FALSE(has(mw, "claude.audit_async"));
    EXPECT_FALSE(has(mw, "claude.mcp_audit_poll"));
    const std::string ci = ciSrc();
    EXPECT_FALSE(has(ci, "claude.audit_async"));
}
