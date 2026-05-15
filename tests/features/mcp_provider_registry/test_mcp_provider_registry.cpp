// Source-grep harness for ANTS-1253 — locks the registry-consolidation
// contract that collapsed 12 setX/m_xProvider pairs into a single
// registerToolProvider + m_toolProviders surface. See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "../../_support/expect.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}



// Counts non-overlapping regex matches across a string (regex has no
// trivial std equivalent for this; the iteration form below is the
// std-recommended pattern).
size_t countMatches(const std::string &haystack, const std::regex &re) {
    auto begin = std::sregex_iterator(haystack.begin(), haystack.end(), re);
    auto end   = std::sregex_iterator();
    return static_cast<size_t>(std::distance(begin, end));
}

}  // namespace

TEST(McpProviderRegistry, Inv1NoStdFunctionProviderMembers) {
    expect_reset();
    // Spec INV-5: 0 std::function<...> m_*Provider members in
    // claudeintegration.h. Catches accidental re-introduction of
    // a per-tool member after the registry consolidation.
    const std::string hdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    std::regex memberRe(R"(std::function\s*<[^>]*>\s+m_\w+Provider\b)");
    const size_t n = countMatches(hdr, memberRe);
    expect(n == 0, "INV-1",
           ("claudeintegration.h has " + std::to_string(n) +
            " std::function<...> m_*Provider members; expected 0 (was 12)")
               .c_str());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv2NoSetXProviderDecls) {
    expect_reset();
    // Setter decls are gone; registry replaces them.
    const std::string hdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    std::regex setterRe(R"(void\s+set[A-Z][A-Za-z]*Provider\b)");
    const size_t n = countMatches(hdr, setterRe);
    expect(n == 0, "INV-2",
           ("claudeintegration.h has " + std::to_string(n) +
            " setXProvider decls; expected 0 (was 12)")
               .c_str());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv3RegistrarAndTypeAlias) {
    expect_reset();
    const std::string hdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    expect(hdr.find("registerToolProvider(const QString &name") != std::string::npos,
           "INV-3a",
           "claudeintegration.h missing registerToolProvider declaration");
    std::regex aliasRe(
        R"(using\s+ToolHandler\s*=\s*std::function\s*<\s*QString\s*\(\s*const\s+QJsonObject\s*&)");
    expect(std::regex_search(hdr, aliasRe),
           "INV-3b",
           "claudeintegration.h missing ToolHandler = "
           "std::function<QString(const QJsonObject&)> alias");
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv4ExplicitMapInclude) {
    expect_reset();
    const std::string hdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    std::regex mapRe(R"(#include\s*<map>)");
    expect(std::regex_search(hdr, mapRe),
           "INV-4",
           "claudeintegration.h missing explicit #include <map>");
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv5DispatcherCollapsed) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    std::regex outwardRe(
        R"((?:else )?if\s*\(\s*toolName\s*==\s*"[^"]+"\s*&&\s*m_\w+Provider\s*\))");
    const size_t outward = countMatches(cpp, outwardRe);
    expect(outward == 0, "INV-5a",
           ("claudeintegration.cpp has " + std::to_string(outward) +
            " outward-delegate dispatch branches; expected 0 (was 12)")
               .c_str());
    std::regex sessionInfoRe(
        R"((?:else )?if\s*\(\s*toolName\s*==\s*"get_session_info"\s*\))");
    const size_t carve = countMatches(cpp, sessionInfoRe);
    expect(carve == 1, "INV-5b",
           ("claudeintegration.cpp has " + std::to_string(carve) +
            " get_session_info inline branches; expected exactly 1 "
            "(the documented carve-out)")
               .c_str());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv6RegistryReferencedFromDispatch) {
    expect_reset();
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    std::regex reg(R"(\bm_toolProviders\b)");
    const size_t n = countMatches(cpp, reg);
    expect(n >= 2, "INV-6",
           ("claudeintegration.cpp references m_toolProviders " +
            std::to_string(n) +
            " times; expected ≥ 2 (registrar body + dispatch lookup)")
               .c_str());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv7AtLeastTwelveRegisterCalls) {
    expect_reset();
    // Spec § 10 step 4 verifier — narrow regex matches the call form
    // (m_claudeIntegration->registerToolProvider("…") and won't drift
    // on future docstring mentions of the function name. ANTS-1253
    // landed with 12 calls; subsequent tools (ANTS-1254 +) add new
    // calls, so the floor is "at least 12".
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    std::regex callRe(R"(->registerToolProvider\(\")");
    const size_t n = countMatches(mw, callRe);
    expect(n >= 12, "INV-7",
           ("mainwindow.cpp has " + std::to_string(n) +
            " ->registerToolProvider(\" call sites; expected ≥ 12")
               .c_str());
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv8SchemaMatchesRegistry) {
    expect_reset();
    // Every tool name in the tools/list schema builder
    // (`<toolVar>["name"] = "<name>"`) other than get_session_info
    // must have a matching registerToolProvider("<name>", …) call
    // in mainwindow.cpp.
    const std::string ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    // Custom raw-string delimiter ("rx") because the regex literal
    // itself contains `)"` which would close the default `R"(…)"` form.
    std::regex schemaRe(R"rx(\["name"\]\s*=\s*"([a-z_]+)")rx");
    std::set<std::string> schemaTools;
    auto begin = std::sregex_iterator(ci.begin(), ci.end(), schemaRe);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) schemaTools.insert((*it)[1].str());
    expect(!schemaTools.empty(), "INV-8a",
           "no tool names found in tools/list schema builder");
    for (const auto &name : schemaTools) {
        if (name == "get_session_info") continue;  // documented carve-out
        const std::string needle = "registerToolProvider(\"" + name + "\"";
        expect(mw.find(needle) != std::string::npos, "INV-8b",
               ("schema names tool \"" + name + "\" but mainwindow.cpp "
                "has no matching registerToolProvider(\"" + name + "\", …) call")
                   .c_str());
    }
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv9GetTextIsDoubleGate) {
    expect_reset();
    // ANTS-1244 INV-9 must survive the consolidation: the get_text
    // lambda body in mainwindow.cpp gates BOTH tab and lines on
    // isDouble() so tab=0 and lines=0 are valid values distinct from
    // omission.
    const std::string mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    const size_t pos = mw.find("registerToolProvider(\"get_text\"");
    expect(pos != std::string::npos, "INV-9a",
           "mainwindow.cpp missing registerToolProvider(\"get_text\", …)");
    if (pos != std::string::npos) {
        const std::string body = mw.substr(pos, 800);
        expect(body.find("isDouble()") != std::string::npos, "INV-9b",
               "get_text lambda body does not use isDouble() to gate "
               "tab/lines extraction (spec INV-3 / ANTS-1244 INV-9)");
        expect(body.find("\"tab\"") != std::string::npos &&
                   body.find("\"lines\"") != std::string::npos,
               "INV-9c",
               "get_text lambda body must reference both \"tab\" and "
               "\"lines\" args");
    }
    EXPECT_EQ(0, expect_failures());
}

TEST(McpProviderRegistry, Inv10SessionInfoCarveoutPreserved) {
    expect_reset();
    // get_session_info is the lone inline branch — it reads
    // ClaudeIntegration's own private state, so it has no provider.
    const std::string cpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const size_t pos = cpp.find("toolName == \"get_session_info\"");
    expect(pos != std::string::npos, "INV-10a",
           "claudeintegration.cpp missing get_session_info inline branch");
    if (pos != std::string::npos) {
        const std::string block = cpp.substr(pos, 600);
        expect(block.find("m_state") != std::string::npos &&
                   block.find("m_currentTool") != std::string::npos &&
                   block.find("m_contextPercent") != std::string::npos,
               "INV-10b",
               "get_session_info inline branch no longer reads "
               "ClaudeIntegration private state (m_state / m_currentTool / "
               "m_contextPercent)");
    }
    EXPECT_EQ(0, expect_failures());
}
