// Feature-conformance test for ANTS-1453 — per-tool selection_hint
// descriptor field on every MCP tool registered through tools/list.
// See tests/features/mcp_selection_hint/spec.md.

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

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

// Capture every `<var>["name"] = "<tool>"` site inside the
// tools/list region, paired with the next descriptor's start.
struct Site {
    std::string tool;
    std::size_t namePos;
    std::size_t nextNamePos;  // start of the following descriptor (or
                              // end-of-region for the last one)
};

std::vector<Site> collectNameSites(const std::string &region) {
    static const std::regex pattern(
        R"((\w+(?:Tool)?|t)\[\"name\"\]\s*=\s*\"([a-z_]+)\")");
    std::vector<Site> out;
    auto begin = std::sregex_iterator(
        region.begin(), region.end(), pattern);
    auto end   = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        Site s;
        s.tool        = (*it)[2].str();
        s.namePos     = static_cast<std::size_t>(it->position());
        s.nextNamePos = region.size();
        out.push_back(s);
    }
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        out[i].nextNamePos = out[i + 1].namePos;
    }
    return out;
}

}  // namespace

// HINT-1 — every registered MCP tool has a selection_hint set in
// the same descriptor block.
TEST(McpSelectionHint, EveryToolDeclaresSelectionHint) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());

    const auto regionStart = cc.find("else if (method == \"tools/list\")");
    ASSERT_NE(regionStart, std::string::npos)
        << "tools/list dispatch branch missing";
    const auto regionEnd = cc.find(
        "else if (method == \"tools/call\")", regionStart);
    ASSERT_NE(regionEnd, std::string::npos)
        << "tools/call branch must follow tools/list";
    const std::string region = cc.substr(
        regionStart, regionEnd - regionStart);

    const auto sites = collectNameSites(region);
    ASSERT_GE(sites.size(), 42u)
        << "ANTS-1453 HINT-1: expected at least 42 registered "
           "tools — descriptor count regression?";

    std::vector<std::string> missing;
    for (const auto &s : sites) {
        const std::string block = region.substr(
            s.namePos, s.nextNamePos - s.namePos);
        if (block.find("\"selection_hint\"") == std::string::npos) {
            missing.push_back(s.tool);
        }
    }
    EXPECT_TRUE(missing.empty())
        << "ANTS-1453 HINT-1: tools registered without "
           "selection_hint: "
        << [&]() {
               std::string joined;
               for (const auto &m : missing) {
                   if (!joined.empty()) joined += ", ";
                   joined += m;
               }
               return joined;
           }();
}

// HINT-2 — tool_info success envelope passes selection_hint through.
TEST(McpSelectionHint, ToolInfoPassesSelectionHint) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());
    // Locate the tool_info inline handler region.
    const auto pos = cc.find("else if (toolName == \"tool_info\")");
    ASSERT_NE(pos, std::string::npos)
        << "tool_info inline handler missing";
    // Window the handler up to the next provider dispatch. ANTS-1985
    // inserted the catalog branch ahead of the single-tool slice, so a
    // fixed-size window no longer reaches the env["selection_hint"]
    // assignment in the slice branch — bound by the handler's end.
    auto end = cc.find("m_toolProviders.find(toolName)", pos);
    if (end == std::string::npos) end = pos + 8000;
    const std::string region = cc.substr(pos, end - pos);

    // Success envelope sets env["name"] / env["description"] /
    // env["inputSchema"]; ANTS-1453 adds env["selection_hint"]
    // sourced from match.value("selection_hint").
    EXPECT_NE(region.find("env[\"selection_hint\"]"),
              std::string::npos)
        << "ANTS-1453 HINT-2: tool_info envelope must set "
           "env[\"selection_hint\"]";
    EXPECT_NE(region.find("match.value(QStringLiteral(\"selection_hint\"))"),
              std::string::npos)
        << "ANTS-1453 HINT-2: tool_info must pull selection_hint "
           "from the cached descriptor without defaulting";
}

// HINT-3 — selection_hint strings stay under ~240 chars each.
TEST(McpSelectionHint, HintsRespectSizeBudget) {
    const std::string cc = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(cc.empty());

    const auto regionStart = cc.find("else if (method == \"tools/list\")");
    ASSERT_NE(regionStart, std::string::npos);
    const auto regionEnd = cc.find(
        "else if (method == \"tools/call\")", regionStart);
    ASSERT_NE(regionEnd, std::string::npos);
    const std::string region = cc.substr(
        regionStart, regionEnd - regionStart);

    // For each selection_hint assignment, collect the concatenated
    // QStringLiteral content (the assignment opens with `(` and
    // closes with `);` possibly several lines later) and assert
    // the total payload length is ≤ 240 chars. std::regex doesn't
    // play nicely with multi-line `[^;]+` runs, so the body is
    // scanned char-by-char with a small state machine instead.
    std::size_t hintCount = 0;
    std::vector<std::string> oversized;
    std::size_t cursor = 0;
    const std::string marker = "\"selection_hint\"]";
    while (true) {
        const std::size_t m = region.find(marker, cursor);
        if (m == std::string::npos) break;
        // Walk past `] = QStringLiteral(` to the opening paren.
        const std::size_t openParen = region.find('(', m);
        if (openParen == std::string::npos) break;
        // Sanity-check the substring between marker and `(` matches
        // ` = QStringLiteral(` modulo whitespace; if not, skip
        // (e.g. the env["selection_hint"] = match.value(...) line).
        const std::string between = region.substr(
            m + marker.size(), openParen - (m + marker.size()));
        if (between.find("QStringLiteral") == std::string::npos) {
            cursor = openParen + 1;
            continue;
        }
        // Walk forward to the matching `);` (top-level), collecting
        // chars inside double-quoted runs.
        std::string text;
        bool inStr = false;
        bool esc   = false;
        int  depth = 1;
        std::size_t j = openParen + 1;
        for (; j < region.size() && depth > 0; ++j) {
            const char c = region[j];
            if (esc) {
                text.push_back(c);
                esc = false;
                continue;
            }
            if (inStr) {
                if (c == '\\') { esc = true; continue; }
                if (c == '"')  { inStr = false; continue; }
                text.push_back(c);
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '(') ++depth;
            if (c == ')') { --depth; if (depth == 0) break; }
        }
        ++hintCount;
        if (text.size() > 240) {
            oversized.push_back(
                std::to_string(text.size()) + " chars: " +
                text.substr(0, 60) + "…");
        }
        cursor = j;
    }
    EXPECT_GE(hintCount, 42u)
        << "ANTS-1453 HINT-3: expected at least 42 hint "
           "assignments; got "
        << hintCount;
    EXPECT_TRUE(oversized.empty())
        << "ANTS-1453 HINT-3: selection_hint > 240 chars: "
        << [&]() {
               std::string joined;
               for (const auto &s : oversized) {
                   joined += "\n  - " + s;
               }
               return joined;
           }();
}
