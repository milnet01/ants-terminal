// ANTS-3585 — feature-conformance test for audit_run's incompleteness
// detail (incompleteToolsDetail helper), cppcheck per-file parse-failure
// surface (parseWithSuppression → parseFailureFiles + the byTool union),
// the raised time ceilings, and the two envelope surfaces.

#include "auditrunner.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

AuditRunner::ToolResult tr(const QString &tool, const QString &status,
                           qint64 elapsedMs = 0) {
    AuditRunner::ToolResult t;
    t.tool      = tool;
    t.status    = status;
    t.elapsedMs = elapsedMs;
    return t;
}

}  // namespace

// INV-1 — incompleteToolsDetail: one {tool,status,elapsed_ms,truncated}
// object per non-ok tool, sorted by name; truncated == timed_out.
TEST(AuditRunIncompleteDetail, Inv1DetailShapeAndTruncatedFlag) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    byTool["semgrep"]  = tr("semgrep", "timed_out", 60000);
    byTool["cppcheck"] = tr("cppcheck", "crashed", 512);
    byTool["ruff"]     = tr("ruff", "ok", 40);
    const QJsonArray d =
        AuditRunner::internal::incompleteToolsDetail(byTool);
    ASSERT_EQ(d.size(), 2);
    // sorted by tool name: cppcheck before semgrep; ruff (ok) excluded.
    const QJsonObject a = d.at(0).toObject();
    EXPECT_EQ(a.value("tool").toString(), QStringLiteral("cppcheck"));
    EXPECT_EQ(a.value("status").toString(), QStringLiteral("crashed"));
    EXPECT_EQ(a.value("elapsed_ms").toInt(), 512);
    EXPECT_FALSE(a.value("truncated").toBool());  // crashed ≠ truncated
    const QJsonObject b = d.at(1).toObject();
    EXPECT_EQ(b.value("tool").toString(), QStringLiteral("semgrep"));
    EXPECT_EQ(b.value("status").toString(), QStringLiteral("timed_out"));
    EXPECT_EQ(b.value("elapsed_ms").toInt(), 60000);
    EXPECT_TRUE(b.value("truncated").toBool());   // timed_out ⇒ truncated
}

// INV-1b — an all-ok map yields an empty detail array.
TEST(AuditRunIncompleteDetail, Inv1AllOkEmpty) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    byTool["ruff"]   = tr("ruff", "ok");
    byTool["bandit"] = tr("bandit", "ok");
    EXPECT_TRUE(
        AuditRunner::internal::incompleteToolsDetail(byTool).isEmpty());
}

// INV-2 — cppcheck parse-failure extraction: a finding whose trailing [id]
// is a parse-failure id records its file; a normal [nullPointer] finding
// does not; the same file failing twice appears once.
TEST(AuditRunIncompleteDetail, Inv2CppcheckParseFailureFiles) {
    const QString raw = QStringLiteral(
        "src/r_vulkan.cpp:100:1: error: Code 'x' is invalid C++ code. "
        "[syntaxError]\n"
        "src/r_vulkan.cpp:250:3: error: Analysis failed. [internalError]\n"
        "src/good.cpp:12:5: error: Null pointer dereference. [nullPointer]\n");
    const AuditRunner::internal::ParsedCounts c =
        AuditRunner::internal::parseWithSuppression(
            QStringLiteral("cppcheck"), raw, 10, {});
    // r_vulkan.cpp (deduped across syntaxError + internalError) is the only
    // parse failure; good.cpp's nullPointer is a real finding, not a failure.
    ASSERT_EQ(c.parseFailureFiles.size(), 1);
    EXPECT_EQ(c.parseFailureFiles.at(0), QStringLiteral("src/r_vulkan.cpp"));
}

// INV-3 — parseFailureFiles(byTool) unions every tool's parseFailureFiles,
// deduped + sorted.
TEST(AuditRunIncompleteDetail, Inv3ByToolUnionSorted) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    AuditRunner::ToolResult cc = tr("cppcheck", "ok");
    cc.parseFailureFiles = {QStringLiteral("src/b.cpp"),
                            QStringLiteral("src/a.cpp")};
    AuditRunner::ToolResult cz = tr("clazy", "ok");
    cz.parseFailureFiles = {QStringLiteral("src/a.cpp")};  // dup across tools
    byTool["cppcheck"] = cc;
    byTool["clazy"]    = cz;
    const QStringList u =
        AuditRunner::internal::parseFailureFiles(byTool);
    ASSERT_EQ(u.size(), 2);
    EXPECT_EQ(u.at(0), QStringLiteral("src/a.cpp"));  // sorted, deduped
    EXPECT_EQ(u.at(1), QStringLiteral("src/b.cpp"));
}

// INV-7 (ANTS-3706) — the reason is captured alongside the file: check-id
// plus the diagnostic text, and the FIRST diagnostic wins (later lines on the
// same TU are cascade noise). A bare path cannot tell a fixable missing
// include path from a frontend limitation; this can.
TEST(AuditRunIncompleteDetail, Inv7ParseFailureReasonCaptured) {
    const QString raw = QStringLiteral(
        "src/r_vulkan.cpp:100:1: error: There is an unknown macro here "
        "somewhere. Configuration is required. If VK_KHR is a macro then "
        "please configure it. [unknownMacro]\n"
        "src/r_vulkan.cpp:12:10: error: Include file: \"SDL.h\" not found. "
        "[preprocessorErrorDirective]\n"
        "src/r_vulkan.cpp:250:3: error: Analysis failed. [internalError]\n");
    const AuditRunner::internal::ParsedCounts c =
        AuditRunner::internal::parseWithSuppression(
            QStringLiteral("cppcheck"), raw, 10, {});
    ASSERT_EQ(c.parseFailureFiles.size(), 1);
    const QString reason =
        c.parseFailureReasons.value(QStringLiteral("src/r_vulkan.cpp"));
    // [unknownMacro] is not a parse-failure id, so the FIRST recorded reason
    // is the preprocessor one — and it names the missing header, which is the
    // whole point of the field.
    EXPECT_TRUE(reason.startsWith(QStringLiteral("preprocessorErrorDirective: ")))
        << "reason was: " << reason.toStdString();
    EXPECT_TRUE(reason.contains(QStringLiteral("SDL.h")))
        << "reason was: " << reason.toStdString();
}

// INV-8 (ANTS-3706) — parseFailureDetails(byTool) emits one {file, tool,
// reason} row per (file, tool), sorted by file then tool. A file two tools
// both failed on yields TWO rows (the reasons differ), unlike the deduped
// parseFailureFiles union.
TEST(AuditRunIncompleteDetail, Inv8ParseFailureDetailsShape) {
    QHash<QString, AuditRunner::ToolResult> byTool;
    AuditRunner::ToolResult cc = tr("cppcheck", "ok");
    cc.parseFailureFiles = {QStringLiteral("src/b.cpp"),
                            QStringLiteral("src/a.cpp")};
    cc.parseFailureReasons.insert(QStringLiteral("src/a.cpp"),
                                  QStringLiteral("syntaxError: bad"));
    AuditRunner::ToolResult cz = tr("clazy", "ok");
    cz.parseFailureFiles = {QStringLiteral("src/a.cpp")};
    byTool["cppcheck"] = cc;
    byTool["clazy"]    = cz;

    const QJsonArray d = AuditRunner::internal::parseFailureDetails(byTool);
    ASSERT_EQ(d.size(), 3);
    // (file, tool) ascending: a.cpp/clazy, a.cpp/cppcheck, b.cpp/cppcheck.
    EXPECT_EQ(d.at(0).toObject().value("file").toString(),
              QStringLiteral("src/a.cpp"));
    EXPECT_EQ(d.at(0).toObject().value("tool").toString(),
              QStringLiteral("clazy"));
    EXPECT_EQ(d.at(1).toObject().value("tool").toString(),
              QStringLiteral("cppcheck"));
    EXPECT_EQ(d.at(1).toObject().value("reason").toString(),
              QStringLiteral("syntaxError: bad"));
    EXPECT_EQ(d.at(2).toObject().value("file").toString(),
              QStringLiteral("src/b.cpp"));
    // No reason recorded for that row — the key is omitted, not empty.
    EXPECT_FALSE(d.at(2).toObject().contains(QStringLiteral("reason")));
}

// INV-9 (ANTS-3706) — both providers emit the detail sibling, and
// parse_failures[] keeps its bare-path shape (no consumer break).
TEST(AuditRunIncompleteDetail, Inv9BothProvidersEmitDetail) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_TRUE(contains(mw, "parse_failures_detail"))
        << "INV-9: sync provider emits parse_failures_detail";
    EXPECT_TRUE(contains(ci, "parse_failures_detail"))
        << "INV-9: async-poll done-branch emits parse_failures_detail";
}

// INV-4 — extraction is cppcheck-gated: the same [syntaxError] line under a
// non-cppcheck tool yields no parse failures (different id namespace).
TEST(AuditRunIncompleteDetail, Inv4NonCppcheckToolGated) {
    const QString raw = QStringLiteral(
        "src/foo.cpp:1:1: warning: something [syntaxError]\n");
    const AuditRunner::internal::ParsedCounts c =
        AuditRunner::internal::parseWithSuppression(
            QStringLiteral("clazy"), raw, 10, {});
    EXPECT_TRUE(c.parseFailureFiles.isEmpty());
}

// INV-5 — the time ceilings are raised above the old 60 s / 240 s so a long
// async sweep can finish; the RunRequest default stays 30 s.
TEST(AuditRunIncompleteDetail, Inv5RaisedCeilings) {
    const std::string rn = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    EXPECT_TRUE(contains(rn, "kCapPerToolMax             = 300"))
        << "INV-5: per-tool cap ceiling raised to 300 s";
    // ANTS-3611 moved kAggregateCapMs to auditrunner.h — assert the real
    // value instead of scraping the .cpp for a byte-exact literal.
    EXPECT_EQ(AuditRunner::kAggregateCapMs, 900'000)
        << "INV-5: aggregate cap ceiling raised to 900 s";
    // Default per-tool cap unchanged (30 s) — the high cap is opt-in.
    const std::string hdr = ants_test::slurpFile(SRC_AUDITRUNNER_H_PATH);
    EXPECT_TRUE(contains(hdr, "capPerToolSeconds = 30"))
        << "INV-5: default per-tool cap unchanged at 30 s";
}

// INV-6 — both envelope surfaces serialise the new fields.
TEST(AuditRunIncompleteDetail, Inv6EnvelopeSurfaces) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    EXPECT_TRUE(contains(mw, "incomplete_tools_detail"))
        << "INV-6: sync provider emits incomplete_tools_detail";
    EXPECT_TRUE(contains(mw, "parse_failures"))
        << "INV-6: sync provider emits parse_failures";
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    EXPECT_TRUE(contains(ci, "incomplete_tools_detail"))
        << "INV-6: async-poll done-branch emits incomplete_tools_detail";
    EXPECT_TRUE(contains(ci, "parse_failures"))
        << "INV-6: async-poll done-branch emits parse_failures";
}
