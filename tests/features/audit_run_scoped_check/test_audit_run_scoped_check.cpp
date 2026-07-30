// Feature-conformance test for spec.md (ANTS-1512).
//
// Source-grep verification that the audit_run scoped-check mode is
// wired into the engine (auditrunner.{h,cpp}), the MCP provider
// (mainwindow.cpp), and the schema descriptor (claudeintegration.cpp).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(AuditRunScopedCheck, EngineHeaderDeclaresPathsAndChecks) {
    const std::string hdr = ants_test::slurpFile(SRC_AUDITRUNNER_H_PATH);
    ASSERT_FALSE(hdr.empty());

    // INV-1 — RunRequest carries paths AND checks fields.
    EXPECT_TRUE(contains(hdr, "QStringList paths"))
        << "RunRequest.paths field missing";
    EXPECT_TRUE(contains(hdr, "QStringList checks"))
        << "RunRequest.checks field missing";
}

TEST(AuditRunScopedCheck, EngineRegistersClangTidyAsKnownTool) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-2 — kKnownTools() contains "clang-tidy". ANTS-1474 — brace-
    // balanced body extraction instead of a fixed byte-window.
    const std::string region =
        ants_test::slurpFunctionBody(src, "kKnownTools()");
    ASSERT_FALSE(region.empty());
    EXPECT_TRUE(contains(region, "\"clang-tidy\""))
        << "kKnownTools must include \"clang-tidy\"";

    // INV-3 — toolHonoursChecks predicate present + returns true for
    // clang-tidy.
    EXPECT_TRUE(contains(src, "toolHonoursChecks"))
        << "toolHonoursChecks predicate missing";
    const auto th = src.find("toolHonoursChecks(");
    ASSERT_NE(th, std::string::npos);
    const std::string thRegion = src.substr(th, 400);
    EXPECT_TRUE(contains(thRegion, "\"clang-tidy\""))
        << "toolHonoursChecks body must whitelist clang-tidy";
}

TEST(AuditRunScopedCheck, CheckSanitiserPresent) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-4 — isAuditCheckSafe with the documented regex.
    EXPECT_TRUE(contains(src, "isAuditCheckSafe"))
        << "isAuditCheckSafe sanitiser missing";
    EXPECT_TRUE(contains(src, "^-?[A-Za-z0-9_*.,-]+$"))
        << "isAuditCheckSafe regex literal missing or changed";
}

TEST(AuditRunScopedCheck, RunAuditValidatesPathsAndChecks) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-5 + INV-6 — runAudit walks req.paths + req.checks and refuses
    // with code:"bad_args" on unsafe entries or tool/check mismatch.
    const auto fn = src.find("runAudit(const RunRequest");
    ASSERT_NE(fn, std::string::npos);
    // Body window: 6 KB.
    const std::string fnRegion = src.substr(fn, 6000);
    EXPECT_TRUE(contains(fnRegion, "for (const QString &p : req.paths)"))
        << "runAudit must iterate req.paths";
    EXPECT_TRUE(contains(fnRegion, "for (const QString &c : req.checks)"))
        << "runAudit must iterate req.checks";
    EXPECT_TRUE(contains(fnRegion, "isAuditCheckSafe(c)"))
        << "runAudit must validate each check entry";
    EXPECT_TRUE(contains(fnRegion, "toolHonoursChecks(t)"))
        << "runAudit must enforce the tool/check compatibility gate";
}

TEST(AuditRunScopedCheck, ToolArgvBuildsScopedClangTidyInvocation) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-7 — toolArgv accepts scopedPaths + scopedChecks; clang-tidy
    // branch renders --checks=-*,...
    // NB: toolArgv's signature carries `= {}` default-argument braces, so
    // slurpFunctionBody() would latch onto the first `{}` default and return
    // an empty body. A fixed byte-window is the correct tool here (ANTS-1474
    // does not apply to brace-default signatures). Window widened
    // 4000 → 5000 by ANTS-2185, whose scoped-positional guard added a
    // normalisation block at the top of toolArgv that pushed the
    // clang-tidy branch (`--checks=-*,`) past the old 4000-byte edge.
    // Widened 5000 → 6500 by ANTS-2182, whose cppcheck compile-DB +
    // suppression block grew the branch above clang-tidy by ~30 lines.
    // Widened 6500 → 8000 by ANTS-3710, whose `excludePaths` parameter and
    // cppcheck `-i` block put `--checks=-*,` at offset 6647 — 147 bytes past
    // the old edge. The new figure is measured, not guessed, and carries
    // headroom so the next branch added above clang-tidy does not re-break it.
    const auto fn = src.find("QStringList toolArgv(");
    ASSERT_NE(fn, std::string::npos);
    const std::string region = src.substr(fn, 8000);
    EXPECT_TRUE(contains(region, "scopedPaths"));
    EXPECT_TRUE(contains(region, "scopedChecks"));
    EXPECT_TRUE(contains(region, "--checks=-*,"))
        << "clang-tidy branch should render the comma-joined --checks=-*";
    EXPECT_TRUE(contains(region, "\"clang-tidy\""))
        << "toolArgv must have a clang-tidy branch";
}

TEST(AuditRunScopedCheck, MainWindowProviderExtractsParams) {
    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());

    // Locate the audit_run provider lambda.
    const auto reg = mw.find("registerToolProvider(\"audit_run\"");
    ASSERT_NE(reg, std::string::npos);
    const std::string region = mw.substr(reg, 5000);

    // INV-8 — extract both paths and checks from args.
    EXPECT_TRUE(contains(region, "\"paths\""))
        << "audit_run provider must extract paths array from args";
    EXPECT_TRUE(contains(region, "\"checks\""))
        << "audit_run provider must extract checks array from args";
    EXPECT_TRUE(contains(region, "req.paths.append"));
    EXPECT_TRUE(contains(region, "req.checks.append"));
}

TEST(AuditRunScopedCheck, DescriptorDeclaresPropertiesAndClangTidy) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());

    // Locate audit_run descriptor block.
    const auto name = ci.find("t[\"name\"] = \"audit_run\"");
    ASSERT_NE(name, std::string::npos);
    const auto end = ci.find("tools.append(t);", name);
    ASSERT_NE(end, std::string::npos);
    const std::string block = ci.substr(name, end - name);

    // INV-9 — paths + checks properties present.
    EXPECT_TRUE(contains(block, "pathsProp"))
        << "descriptor must declare a pathsProp schema entry";
    EXPECT_TRUE(contains(block, "checksProp"))
        << "descriptor must declare a checksProp schema entry";
    EXPECT_TRUE(contains(block, "props[\"paths\"]"));
    EXPECT_TRUE(contains(block, "props[\"checks\"]"));

    // tools description should mention clang-tidy now.
    EXPECT_TRUE(contains(block, "clang-tidy"))
        << "tools description must mention clang-tidy after ANTS-1512";
}
