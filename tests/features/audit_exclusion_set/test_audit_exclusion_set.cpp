// Feature-conformance test for tests/features/audit_exclusion_set/spec.md.
//
// ANTS-1709 — centralised audit directory-exclusion set + the openUrl
// `fromLocalFile` idiom. Behavioural checks on the AuditEngine formatters
// plus source-grep drift guards on the two consumers (auditdialog.cpp,
// featurecoverage.cpp).

#include "auditengine.h"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_AUDIT_CPP_PATH
#error "SRC_AUDIT_CPP_PATH compile definition required"
#endif
#ifndef SRC_FEATURECOVERAGE_CPP_PATH
#error "SRC_FEATURECOVERAGE_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool has(const QString &hay, const char *needle) {
    return hay.contains(QLatin1String(needle));
}

}  // namespace

// INV-1
TEST(AuditExclusionSet, CanonicalNamesPresentBuildExcluded) {
    const QStringList names = AuditEngine::excludedDirNames();
    for (const char *d : {"node_modules", "vendor", ".audit_cache",
                          "__pycache__", "Testing", ".git", ".venv"}) {
        EXPECT_TRUE(names.contains(QLatin1String(d)))
            << "excludedDirNames() missing " << d;
    }
    // `build` is the globbed family; the per-tool formatters add it.
    EXPECT_FALSE(names.contains(QStringLiteral("build")))
        << "build must be handled as a glob by the formatters, not listed";
}

// INV-2
TEST(AuditExclusionSet, FindExprCoversBuildGlobAndNestedPycache) {
    const QString e = AuditEngine::findExcludeExpr();
    EXPECT_TRUE(has(e, "-not -path './build/*'"));
    EXPECT_TRUE(has(e, "-not -path './build-*/*'"));      // build-fast / -workstation
    EXPECT_TRUE(has(e, "-not -path '*/__pycache__/*'"));  // any depth
    EXPECT_TRUE(has(e, "-not -path './node_modules/*'"));
}

// INV-3
TEST(AuditExclusionSet, GrepExprCoversBuildGlob) {
    const QString e = AuditEngine::grepExcludeExpr();
    EXPECT_TRUE(has(e, "--exclude-dir=build"));
    EXPECT_TRUE(has(e, "--exclude-dir='build-*'"));
    EXPECT_TRUE(has(e, "--exclude-dir=node_modules"));
}

// INV-4 — locks the ANTS-1707-class regression (static list misses presets).
TEST(AuditExclusionSet, TrivyCsvIsGlobBased) {
    const QString csv = AuditEngine::trivySkipDirsCsv();
    EXPECT_TRUE(has(csv, "build-*"))
        << "trivy --skip-dirs must use the build-* glob, not a static list "
           "that silently misses build-fast / build-workstation";
    EXPECT_TRUE(has(csv, "node_modules"));
    EXPECT_TRUE(has(csv, "vendor"));
    EXPECT_FALSE(has(csv, "build-test,build-release,build-asan"))
        << "the drift-prone static enumeration must be gone";
}

// INV-5
TEST(AuditExclusionSet, CppcheckExprExpandsAtRuntime) {
    const QString e = AuditEngine::cppcheckIgnoreShellExpr();
    EXPECT_TRUE(has(e, "for d in build build-*"));
    EXPECT_TRUE(has(e, "printf -- '-i %s '"));
}

// INV-6 — auditdialog.cpp routes through the engine; no inline copies survive.
TEST(AuditExclusionSet, DialogRoutesThroughEngine) {
    const std::string src = slurp(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "could not read auditdialog.cpp";
    EXPECT_NE(src.find("AuditEngine::trivySkipDirsCsv()"), std::string::npos);
    EXPECT_NE(src.find("AuditEngine::cppcheckIgnoreShellExpr()"), std::string::npos);
    EXPECT_NE(src.find("AuditEngine::findExcludeExpr()"), std::string::npos);
    EXPECT_NE(src.find("AuditEngine::grepExcludeExpr()"), std::string::npos);
    EXPECT_EQ(src.find("--skip-dirs build,build-test"), std::string::npos)
        << "trivy must not hardcode a static skip-dirs list again";
    EXPECT_EQ(src.find("for d in build build-* node_modules .audit_cache"),
              std::string::npos)
        << "cppcheck must not inline its own build-glob loop again";
}

// INV-7
TEST(AuditExclusionSet, FeatureCoverageDerivesFromEngine) {
    const std::string src = slurp(SRC_FEATURECOVERAGE_CPP_PATH);
    ASSERT_FALSE(src.empty()) << "could not read featurecoverage.cpp";
    EXPECT_NE(src.find("AuditEngine::excludedDirNames()"), std::string::npos)
        << "featurecoverage must derive its skip set from the engine";
}

// INV-8 — openUrl rule recognises the fromLocalFile idiom.
TEST(AuditExclusionSet, OpenUrlRuleModelsFromLocalFile) {
    const std::string src = slurp(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t rule = src.find("qt_openurl_unchecked");
    ASSERT_NE(rule, std::string::npos);
    EXPECT_NE(src.find("fromLocalFile", rule), std::string::npos)
        << "openUrl rule must treat QUrl::fromLocalFile as a validated source";
}

// INV-9 — applyFilter drops a fromLocalFile-bearing openUrl line.
TEST(AuditExclusionSet, ApplyFilterDropsFromLocalFileLine) {
    OutputFilter f;
    f.dropIfContains = {QStringLiteral("fromLocalFile")};
    f.maxLines = 20;
    const QString raw = QStringLiteral(
        "src/foo.cpp:42: QDesktopServices::openUrl(QUrl::fromLocalFile(p));\n");
    const AuditEngine::FilterResult r =
        AuditEngine::applyFilter(raw, f, QStringLiteral("."));
    EXPECT_EQ(r.count, 0)
        << "fromLocalFile line should be dropped; body was: "
        << r.body.toStdString();
}

// INV-10 — ANTS-1707 regression lock: header_guards probes #pragma once
// whole-file BEFORE the head-limited #ifndef probe.
TEST(AuditExclusionSet, HeaderGuardsMatchesPragmaOnceWholeFile) {
    const std::string src = slurp(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t rule = src.find("header_guards");
    ASSERT_NE(rule, std::string::npos);
    const size_t pragma = src.find("#pragma[[:space:]]+once", rule);
    const size_t headLimited = src.find("head -50", rule);
    ASSERT_NE(pragma, std::string::npos)
        << "header_guards must grep for #pragma once";
    ASSERT_NE(headLimited, std::string::npos);
    EXPECT_LT(pragma, headLimited)
        << "the #pragma once probe must run whole-file, before the "
           "head-limited #ifndef probe (else a guard behind a long "
           "doc comment reads as 'unguarded' — the ANTS-1707 FP)";
}

// INV-11 — ANTS-1710: insecure_http rule models license/spec URLs as docs.
TEST(AuditExclusionSet, InsecureHttpDropsLicenseUrls) {
    const std::string src = slurp(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t rule = src.find("insecure_http");
    ASSERT_NE(rule, std::string::npos);
    EXPECT_NE(src.find("apache.org/licenses", rule), std::string::npos)
        << "insecure_http must drop Apache license-header URLs";
    EXPECT_NE(src.find("gnu.org/licenses", rule), std::string::npos)
        << "insecure_http must drop GNU license-header URLs";
}

// INV-12 — applyFilter drops a license-header http:// line.
TEST(AuditExclusionSet, ApplyFilterDropsLicenseUrlLine) {
    OutputFilter f;
    f.dropIfContains = {QStringLiteral("apache.org/licenses")};
    f.maxLines = 30;
    const QString raw = QStringLiteral(
        "src/foo.cpp:3: // http://www.apache.org/licenses/LICENSE-2.0\n");
    const AuditEngine::FilterResult r =
        AuditEngine::applyFilter(raw, f, QStringLiteral("."));
    EXPECT_EQ(r.count, 0)
        << "license-header URL line should be dropped; body was: "
        << r.body.toStdString();
}

// INV-13 — ANTS-1710: secrets_scan models the env-read idiom as safe.
TEST(AuditExclusionSet, SecretsScanDropsEnvReadIdiom) {
    const std::string src = slurp(SRC_AUDIT_CPP_PATH);
    ASSERT_FALSE(src.empty());
    const size_t rule = src.find("secrets_scan");
    ASSERT_NE(rule, std::string::npos);
    EXPECT_NE(src.find("qEnvironmentVariable", rule), std::string::npos)
        << "secrets_scan must treat qEnvironmentVariable reads as non-secrets";
    EXPECT_NE(src.find("getenv", rule), std::string::npos)
        << "secrets_scan must treat getenv reads as non-secrets";
}

// INV-14 — applyFilter drops a secret-from-env line.
TEST(AuditExclusionSet, ApplyFilterDropsEnvReadSecretLine) {
    OutputFilter f;
    f.dropIfContains = {QStringLiteral("qEnvironmentVariable")};
    f.maxLines = 50;
    const QString raw = QStringLiteral(
        "src/foo.cpp:9: password = qEnvironmentVariable(\"APP_PASSWORD\");\n");
    const AuditEngine::FilterResult r =
        AuditEngine::applyFilter(raw, f, QStringLiteral("."));
    EXPECT_EQ(r.count, 0)
        << "env-read secret line should be dropped; body was: "
        << r.body.toStdString();
}
