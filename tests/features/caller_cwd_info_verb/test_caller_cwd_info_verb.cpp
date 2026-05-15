// Feature-conformance test for ANTS-1400 — caller_cwd_info MCP verb.
// See tests/features/caller_cwd_info_verb/spec.md.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

namespace {

std::string slurp(const char *p) {
    std::ifstream in(p);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// REG-1 — verb registered in mainwindow.cpp.
TEST(CallerCwdInfoVerb, RegisteredAsMcpTool) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    EXPECT_NE(mw.find("registerToolProvider(\"caller_cwd_info\""),
              std::string::npos)
        << "ANTS-1400 REG-1: caller_cwd_info not registered "
           "as an MCP tool in mainwindow.cpp";
}

// REG-2 — handler delegates to resolveCallerCwdRoot and emits
// the four envelope fields.
TEST(CallerCwdInfoVerb, HandlerDelegatesToHelper) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const auto pos = mw.find("registerToolProvider(\"caller_cwd_info\"");
    ASSERT_NE(pos, std::string::npos);
    // Take the next ~600 bytes — captures the lambda body without
    // bleeding into the following registration.
    const std::string region = mw.substr(pos, 600);
    EXPECT_NE(region.find("ants::resolveCallerCwdRoot"), std::string::npos)
        << "ANTS-1400 REG-2: handler must delegate to "
           "ants::resolveCallerCwdRoot";
    EXPECT_NE(region.find("\"source\""), std::string::npos)
        << "ANTS-1400 REG-2: source field missing from envelope";
    EXPECT_NE(region.find("\"resolved_cwd\""), std::string::npos)
        << "ANTS-1400 REG-2: resolved_cwd field missing";
    EXPECT_NE(region.find("\"tab_index\""), std::string::npos)
        << "ANTS-1400 REG-2: tab_index field missing";
    EXPECT_NE(region.find("\"ok\""), std::string::npos)
        << "ANTS-1400 REG-2: ok field missing";
    EXPECT_NE(region.find("sourceToString"), std::string::npos)
        << "ANTS-1400 REG-2: source field must serialise via "
           "sourceToString";
}

// REG-3 — schema block lists the verb with caller_cwd and an
// empty required array.
TEST(CallerCwdInfoVerb, SchemaListed) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto pos = cc.find("t[\"name\"] = \"caller_cwd_info\"");
    ASSERT_NE(pos, std::string::npos)
        << "ANTS-1400 REG-3: tool descriptor missing for caller_cwd_info";
    // 2000-byte window covers the description's multi-line literal
    // (~11 wrapped lines @ ~80 chars each) without bleeding into
    // the following session_memory descriptor.
    const std::string region = cc.substr(pos, 2000);
    EXPECT_NE(region.find("props[\"caller_cwd\"]"),
              std::string::npos)
        << "ANTS-1400 REG-3: caller_cwd property missing from schema";
    EXPECT_NE(region.find("schema[\"required\"] = QJsonArray();"),
              std::string::npos)
        << "ANTS-1400 REG-3: required array must be empty — the "
           "verb's whole job is to accept empty caller_cwd as a "
           "valid input";
}

// REG-4 — classification = Optional. Anchored to the
// `callerCwdContractFor` body so the schema descriptor's
// `t["name"] = "caller_cwd_info"` line (the first occurrence in
// the file) doesn't shadow the classification check.
TEST(CallerCwdInfoVerb, ClassifiedAsOptional) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    const auto helperPos = cc.find(
        "callerCwdContractFor(const QString &toolName)");
    ASSERT_NE(helperPos, std::string::npos)
        << "ANTS-1400 REG-4: callerCwdContractFor helper missing";
    const auto helperEnd = cc.find("\n}\n", helperPos);
    ASSERT_NE(helperEnd, std::string::npos);
    const std::string body = cc.substr(helperPos, helperEnd - helperPos);

    const auto pos = body.find("\"caller_cwd_info\"");
    ASSERT_NE(pos, std::string::npos)
        << "ANTS-1400 REG-4: caller_cwd_info missing from "
           "callerCwdContractFor classification table";
    const auto eol = body.find('\n', pos);
    ASSERT_NE(eol, std::string::npos);
    const std::string line = body.substr(pos, eol - pos);
    EXPECT_NE(line.find("C::Optional"), std::string::npos)
        << "ANTS-1400 REG-4: caller_cwd_info must be classified "
           "Optional — Required would refuse empty caller_cwd, "
           "defeating the diagnostic purpose";
}

// REG-5 — sourceToString enumerates all four Source values.
TEST(CallerCwdInfoVerb, SourceToStringCoversAllValues) {
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    const auto pos = mw.find("sourceToString(ants::ResolvedRoot::Source");
    ASSERT_NE(pos, std::string::npos)
        << "ANTS-1400 REG-5: sourceToString helper missing";
    const std::string region = mw.substr(pos, 800);
    for (const auto *val : {
            "S::ExplicitMatch",
            "S::EmptyFallback",
            "S::NoMatch",
            "S::Unresolvable"}) {
        EXPECT_NE(region.find(val), std::string::npos)
            << "ANTS-1400 REG-5: missing case " << val
            << " in sourceToString";
    }
}
