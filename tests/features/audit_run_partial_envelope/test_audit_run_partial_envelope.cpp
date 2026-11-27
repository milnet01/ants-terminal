// ANTS-2032 — feature-conformance test for audit_run's partiality
// surface (incompleteToolNames helper + partial flag derivation) and
// the SARIF-before-reply / partial-envelope source guarantees.

#include "auditrunner.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include <string>

#include <unistd.h>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

AuditRunner::ToolResult tr(const QString &tool, const QString &status) {
    AuditRunner::ToolResult t;
    t.tool   = tool;
    t.status = status;
    return t;
}

}  // namespace

// INV-1 — incompleteToolNames lists non-ok tools, sorted ascending.
TEST(AuditRunPartialEnvelope, Inv1IncompleteNamesSorted) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    byTool["semgrep"]  = tr("semgrep", "timed_out");
    byTool["ruff"]     = tr("ruff", "ok");
    byTool["cppcheck"] = tr("cppcheck", "crashed");
    byTool["bandit"]   = tr("bandit", "ok");
    const QStringList inc = AuditRunner::internal::incompleteToolNames(byTool);
    ASSERT_EQ(inc.size(), 2);
    EXPECT_EQ(inc[0], QStringLiteral("cppcheck"));  // sorted
    EXPECT_EQ(inc[1], QStringLiteral("semgrep"));
}

// INV-1b — an all-ok map yields an empty list.
TEST(AuditRunPartialEnvelope, Inv1AllOkEmpty) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    byTool["ruff"]   = tr("ruff", "ok");
    byTool["bandit"] = tr("bandit", "ok");
    EXPECT_TRUE(AuditRunner::internal::incompleteToolNames(byTool).isEmpty());
}

// INV-2 — partial ⟺ a tool did not finish ok (the derivation the runner
// applies: partial = !incompleteTools.isEmpty()).
TEST(AuditRunPartialEnvelope, Inv2PartialDerivation) {
    QHash<QString, AuditRunner::ToolResult> ok;
    ok["ruff"] = tr("ruff", "ok");
    EXPECT_FALSE(!AuditRunner::internal::incompleteToolNames(ok).isEmpty());

    QHash<QString, AuditRunner::ToolResult> mixed;
    mixed["ruff"]    = tr("ruff", "ok");
    mixed["semgrep"] = tr("semgrep", "timed_out");
    EXPECT_TRUE(!AuditRunner::internal::incompleteToolNames(mixed).isEmpty());
}

// INV-3 — the reply builder serialises the partial surface, and the
// descriptor advertises it.
TEST(AuditRunPartialEnvelope, Inv3EnvelopeSerialisesPartial) {
    AuditRunner::RunResult r;
    r.partial         = true;
    r.incompleteTools = {QStringLiteral("cppcheck")};
    const QJsonObject env = AuditRunner::doneEnvelope(r);
    EXPECT_TRUE(env.value(QStringLiteral("partial")).toBool())
        << "INV-3: the reply carries the partial flag";
    EXPECT_EQ(env.value(QStringLiteral("incomplete_tools")).toArray(),
              QJsonArray{QStringLiteral("cppcheck")})
        << "INV-3: the reply lists incomplete_tools[]";
    EXPECT_FALSE(AuditRunner::doneEnvelope(AuditRunner::RunResult{})
                     .contains(QStringLiteral("incomplete_tools")))
        << "INV-3: an empty incomplete_tools is omitted";
    const std::string ci = ants_test::slurpClaudeIntegration();
    EXPECT_TRUE(contains(ci, "incomplete_tools"))
        << "INV-3: audit_run descriptor advertises incomplete_tools";
}

// INV-4 — SARIF is written inside runAudit before it returns (the caller
// serialises the reply only after runAudit returns), so a too-big/failed
// reply still leaves a recoverable artifact. Source-order guarantee.
TEST(AuditRunPartialEnvelope, Inv4SarifWrittenBeforeReturn) {
    const std::string rn = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    const std::string writer =
        ants_test::slurpFunctionBody(rn, "void writeArtifacts(");
    ASSERT_FALSE(writer.empty()) << "writeArtifacts body not found";
    EXPECT_TRUE(contains(writer, "writeSarif(cacheSarifAbs"))
        << "INV-4: writeArtifacts writes the SARIF";
    // runAudit's final `return r;` must come AFTER the artifact write.
    const std::string run =
        ants_test::slurpFunctionBody(rn, "RunResult runAudit(const RunRequest");
    ASSERT_FALSE(run.empty()) << "runAudit body not found";
    const auto writePos = run.find("writeArtifacts(");
    ASSERT_NE(writePos, std::string::npos)
        << "INV-4: runAudit calls writeArtifacts";
    const auto retPos = run.rfind("return r;");
    ASSERT_NE(retPos, std::string::npos);
    EXPECT_LT(writePos, retPos)
        << "INV-4: SARIF is written before runAudit returns (artifact "
           "exists before the caller serialises the reply)";
}

// ANTS-4371 — a zero-finding audit must carry evidence the tools were handed
// work. Five tools over scope:"full" returned total_raw:0, partial:false,
// incomplete_tools:[] and artifacts:0 on every SARIF run, and nothing
// distinguished "ran across 2116 lines and found nothing" from "ran against
// an empty file list". Confirming the sweep was real cost a manual re-run of
// the tools the verb had just run — the cost the verb exists to remove.
//
// It matters because a zero-finding audit is the most consequential result
// this verb returns: it is what lets a phase close.
TEST(AuditRunPartialEnvelope, Ants4371CoverageEvidenceIsSerialised) {
    AuditRunner::RunResult r;
    AuditRunner::ToolResult whole;  // scope:"full": pointed at the root
    whole.status       = QStringLiteral("ok");
    whole.wholeProject = true;
    AuditRunner::ToolResult narrowed;  // a narrowed scope that matched nothing
    narrowed.status = QStringLiteral("ok");
    AuditRunner::ToolResult listed;
    listed.status     = QStringLiteral("ok");
    listed.pathsGiven = 3;
    r.byTool.insert(QStringLiteral("ruff"), whole);
    r.byTool.insert(QStringLiteral("semgrep"), narrowed);
    r.byTool.insert(QStringLiteral("cppcheck"), listed);
    const QJsonObject env = AuditRunner::doneEnvelope(r);
    const QJsonObject byTool = env.value(QStringLiteral("by_tool")).toObject();

    const QJsonObject ruff = byTool.value(QStringLiteral("ruff")).toObject();
    EXPECT_EQ(ruff.value(QStringLiteral("paths_given")).toInt(-1), 0)
        << "per-tool: how many explicit paths the tool was handed";
    EXPECT_TRUE(ruff.value(QStringLiteral("scanned_whole_project")).toBool())
        << "…and whether it was pointed at the root instead. Under "
           "scope:\"full\" paths_given is legitimately 0, so the count ALONE "
           "would report \"scanned nothing\" for exactly the case this "
           "reassures about — both fields together are the answer";
    EXPECT_FALSE(ruff.contains(QStringLiteral("no_files")));
    EXPECT_TRUE(byTool.value(QStringLiteral("semgrep")).toObject()
                    .value(QStringLiteral("no_files")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("paths_given_total")).toInt(), 3);
    EXPECT_TRUE(env.value(QStringLiteral("scanned_whole_project")).toBool());
    EXPECT_EQ(env.value(QStringLiteral("tools_with_no_files")).toArray(),
              QJsonArray{QStringLiteral("semgrep")})
        << "the top-level roll-up: which tools got an explicitly narrowed but "
           "EMPTY list, which scope:files / since-last-run produce legitimately";

    // The evidence must NOT be folded into the partiality signal. A narrowed
    // scope matching no files is a legitimate outcome, and marking it partial
    // would make every narrow scan report a failure it did not have — trading
    // a silent wrong answer for a noisy one.
    EXPECT_FALSE(env.value(QStringLiteral("partial")).toBool());
    EXPECT_FALSE(env.contains(QStringLiteral("incomplete_tools")))
        << "no_files must stay out of incomplete_tools / partial";
}

// INV-5 (ANTS-6981) — when both SARIF writes fail, the reply stays ok with its
// findings, omits sarif_path, and says why in sarif_error. The cache write
// fails on a `.audit_cache` that is a plain file; the fallback fails in a
// read-only fallback directory.
TEST(AuditRunPartialEnvelope, Inv5FailedSarifWriteIsReported) {
    if (::geteuid() == 0) GTEST_SKIP() << "root ignores the read-only directory";
    QTemporaryDir project, cacheHome, emptyBin;
    ASSERT_TRUE(project.isValid() && cacheHome.isValid() && emptyBin.isValid());
    {
        QFile blocker(project.path() + QStringLiteral("/.audit_cache"));
        ASSERT_TRUE(blocker.open(QIODevice::WriteOnly)) << "setup: no .audit_cache file";
    }

    AuditRunner::RunResult r;
    QString fallbackDir;
    {
        ants_test::XdgGuard env;
        env.setEnv("XDG_CACHE_HOME", cacheHome.path().toLocal8Bit());
        env.setEnv("PATH", emptyBin.path().toLocal8Bit());
        const QString cacheRoot =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cacheRoot.isEmpty()) GTEST_SKIP() << "no XDG cache location here";
        fallbackDir = cacheRoot + QStringLiteral("/audit");
        ASSERT_TRUE(QDir().mkpath(fallbackDir)) << "setup: no fallback directory";
        ASSERT_TRUE(QFile::setPermissions(fallbackDir,
                        QFileDevice::ReadOwner | QFileDevice::ExeOwner))
            << "setup: the fallback directory stayed writable";
        AuditRunner::RunRequest req;
        req.projectRoot = project.path();
        r = AuditRunner::runAudit(req);
        QFile::setPermissions(fallbackDir, QFileDevice::ReadOwner
                                  | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    ASSERT_TRUE(r.ok) << "INV-5: the run refused: " << r.code.toStdString();
    EXPECT_FALSE(r.byTool.isEmpty()) << "INV-5: the run kept no findings";
    const QJsonObject env = AuditRunner::doneEnvelope(r);
    EXPECT_FALSE(env.contains(QStringLiteral("sarif_path")))
        << "setup: a SARIF write succeeded";
    const QString why = env.value(QStringLiteral("sarif_error")).toString();
    EXPECT_FALSE(why.isEmpty()) << "INV-5: no sarif_error says why sarif_path is missing";
    EXPECT_TRUE(why.contains(QStringLiteral(".audit_cache")))
        << "INV-5: sarif_error does not name the cache path: " << why.toStdString();
    EXPECT_TRUE(why.contains(fallbackDir))
        << "INV-5: sarif_error does not name the fallback path: " << why.toStdString();
}
