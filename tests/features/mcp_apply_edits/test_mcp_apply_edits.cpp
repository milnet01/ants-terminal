// Feature-conformance test for the apply_edits MCP tool (ANTS-2022).
// Behavioural invariants exercise the pure ApplyEdits::applyToContent
// helper; wiring invariants source-scrape the registration sites.
// See spec.md + docs/specs/ANTS-2022.md.

#include "../../_support/expect.h"
#include "applyedits.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <QString>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const std::string &path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    std::stringstream ss; ss << in.rdbuf(); return ss.str();
}
bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// W1 — wiring across the registration sites.
TEST(McpApplyEdits, WiringContract) {
    expect_reset();
    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);

    expect(has(rcHdr, "cmdApplyEdits(const QJsonObject &req)"), "W1 decl",
           "remotecontrol.h missing cmdApplyEdits declaration");
    expect(has(rcCpp, "cmdApplyEdits") && has(rcCpp, "ApplyEdits::applyToContent"),
           "W1 impl", "remotecontrol.cpp cmdApplyEdits must call ApplyEdits::applyToContent");
    expect(has(rcCpp, "validatePath") && has(rcCpp, "apply_edits"),
           "W1 pathvalidation", "cmdApplyEdits must validate each path arg");
    expect(has(rcCpp, "QSaveFile") && has(rcCpp, "fsyncParentDir"),
           "W1 atomic", "cmdApplyEdits must write atomically (QSaveFile + fsyncParentDir)");
    expect(has(ciCpp, "\"apply_edits\""), "W1 schema",
           "claudeintegration.cpp missing apply_edits registration");
    expect(has(ciCpp, "callerCwdContractFor") &&
           has(ciCpp, "C::Required") && has(ciCpp, "apply_edits"),
           "W1 Required", "apply_edits must be callerCwdContractFor → Required");
    expect(has(ciCpp, "\"edits\""), "W1 prop edits",
           "apply_edits schema missing the edits property");
    expect(has(mwCpp, "registerToolProvider(\"apply_edits\"") &&
           has(mwCpp, "cmdApplyEdits"), "W1 dispatch",
           "mainwindow.cpp must register apply_edits → cmdApplyEdits");
    EXPECT_EQ(0, expect_failures());
}

// B1 — unique / absent / duplicate `old` (INV-1).
TEST(McpApplyEdits, UniqueAbsentDuplicate) {
    // Unique → applied.
    auto u = ApplyEdits::applyToContent("alpha beta gamma", "beta", "BETA", false);
    EXPECT_TRUE(u.applied);
    EXPECT_EQ(u.replacements, 1);
    EXPECT_EQ(u.newContents, "alpha BETA gamma");

    // Absent → not_found.
    auto a = ApplyEdits::applyToContent("alpha beta", "zeta", "Z", false);
    EXPECT_FALSE(a.applied);
    EXPECT_EQ(a.skipReason, "not_found");

    // Duplicate without replace_all → ambiguous.
    auto d = ApplyEdits::applyToContent("x x x", "x", "y", false);
    EXPECT_FALSE(d.applied);
    EXPECT_EQ(d.skipReason, "ambiguous");
}

// B2 — replace_all (INV-2).
TEST(McpApplyEdits, ReplaceAll) {
    auto r = ApplyEdits::applyToContent("a.a.a", "a", "b", true);
    EXPECT_TRUE(r.applied);
    EXPECT_EQ(r.replacements, 3);
    EXPECT_EQ(r.newContents, "b.b.b");

    // replace_all with 0 occurrences still skips not_found (no silent no-op).
    auto z = ApplyEdits::applyToContent("nothing", "q", "Q", true);
    EXPECT_FALSE(z.applied);
    EXPECT_EQ(z.skipReason, "not_found");
}

// B3 — trailing newline preserved by whole-content substring replace (INV-8).
TEST(McpApplyEdits, TrailingNewline) {
    // File ending in a newline keeps exactly one.
    auto withNl = ApplyEdits::applyToContent("foo\nbar\n", "bar", "baz", false);
    ASSERT_TRUE(withNl.applied);
    EXPECT_EQ(withNl.newContents, "foo\nbaz\n");
    EXPECT_TRUE(withNl.newContents.endsWith('\n'));

    // File with no final newline keeps none.
    auto noNl = ApplyEdits::applyToContent("foo\nbar", "bar", "baz", false);
    ASSERT_TRUE(noNl.applied);
    EXPECT_EQ(noNl.newContents, "foo\nbaz");
    EXPECT_FALSE(noNl.newContents.endsWith('\n'));
}
