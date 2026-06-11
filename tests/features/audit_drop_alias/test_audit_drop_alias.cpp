// Feature-conformance test for ANTS-1111 — `// audit: drop` alias for
// the inline-suppress regex. Source-grep approach: read auditdialog.cpp
// and verify the regex source contains the new alternative.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_AUDITDIALOG_PATH
#error "SRC_AUDITDIALOG_PATH compile definition required"
#endif

namespace {


}  // namespace

TEST(AuditDropAlias, Inv9RegexAlternativePresent) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty()) << "could not read auditdialog.cpp";
    EXPECT_NE(src.find("audit:\\s*drop(?:-next-line|-file)?"), std::string::npos)
        << "auditdialog.cpp:2055 regex must contain audit:\\s*drop alternative";
}

TEST(AuditDropAlias, ExistingAntsAuditTokenStillPresent) {
    const std::string src = ants_test::slurpFile(SRC_AUDITDIALOG_PATH);
    ASSERT_FALSE(src.empty());
    EXPECT_NE(src.find("ants-audit:\\s*disable(?:-next-line|-file)?"),
              std::string::npos)
        << "the existing ants-audit:\\s*disable alternative must coexist";
}
