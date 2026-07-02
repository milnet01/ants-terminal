// Feature-conformance test for spec.md (ANTS-3418).
//
// Source-grep verification that audit_run drops mypy from the auto-detect
// default tool set (deps-less mypy = import-not-found false positives) while
// keeping it a KNOWN tool so an explicit tools:["mypy"] still runs.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <string>

namespace {
bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}
}  // namespace

TEST(AuditRunDefaultTools, MypyStaysAKnownTool) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-1 — kKnownTools() still lists mypy, so an explicit tools:["mypy"]
    // passes the allowlist validation and runs.
    const std::string known =
        ants_test::slurpFunctionBody(src, "kKnownTools()");
    ASSERT_FALSE(known.empty());
    EXPECT_TRUE(contains(known, "\"mypy\""))
        << "mypy must remain a known/allowed tool (explicit request path)";
}

TEST(AuditRunDefaultTools, AutoDetectSetDropsMypy) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-2 — kAutoDetectTools() derives from kKnownTools() and removes
    // mypy; this is the set used when the caller omits `tools`.
    const std::string autod =
        ants_test::slurpFunctionBody(src, "kAutoDetectTools()");
    ASSERT_FALSE(autod.empty())
        << "kAutoDetectTools() (the auto-detect default) must exist";
    EXPECT_TRUE(contains(autod, "kKnownTools()"))
        << "auto-detect set must derive from kKnownTools()";
    EXPECT_TRUE(contains(autod, "removeAll") && contains(autod, "\"mypy\""))
        << "auto-detect set must drop mypy";
}

TEST(AuditRunDefaultTools, EmptyToolsDefaultsToAutoDetectSet) {
    const std::string src = ants_test::slurpFile(SRC_AUDITRUNNER_CPP_PATH);
    ASSERT_FALSE(src.empty());

    // INV-3 — the empty-`tools` default in runAudit assigns the auto-detect
    // set (not the full kKnownTools()).
    const auto pos = src.find("wantedTools.isEmpty()");
    ASSERT_NE(pos, std::string::npos)
        << "the empty-tools default assignment is gone/renamed";
    const std::string region = src.substr(pos, 120);
    EXPECT_TRUE(contains(region, "kAutoDetectTools()"))
        << "empty tools must default to kAutoDetectTools(), not kKnownTools()";
}
