// ANTS-2079 — feature-conformance test for the MCP tool
// `description`/`detail` split. Source-scrape pattern (same harness as
// mcp_tool_info_verb): we read src/claudeintegration.cpp and assert the
// structural facts that implement each invariant, plus a reconstructed
// wire-byte budget for INV-5. See spec.md + docs/specs/ANTS-2079.md.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

ANTS_TEST_SCOPE();

namespace {

using ants_test::slurpFile;
using ants_test::squashWhitespace;

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

const std::string &ciSource() {
    static const std::string s =
        slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    return s;
}

// The seven in-scope tools (wire `description` >= ~1800 B pre-trim).
const std::array<const char *, 7> kInScope = {
    "roadmap_query", "model_switch_stats", "roadmap_log",
    "test_audit_partition", "changelog_log", "workspace_search",
    "verify_changes"};

// Etag-supported in-scope tools carry the ~221 B "Etag tip:" memo on the
// wire (appended by the tools/list prefix loop before the snapshot).
bool isEtagTool(const std::string &name) {
    return name == "roadmap_query" || name == "model_switch_stats";
}

// Byte length of the decoded "Etag tip:" memo the prefix loop appends
// (claudeintegration.cpp ~8087). The em-dash is the 3-byte UTF-8 U+2014.
std::size_t etagTipBytes() {
    static const std::string memo =
        " Etag tip: cache the returned `etag` field and pass it back via "
        "`etag_match` on subsequent calls in the same session "
        "\xe2\x80\x94 saves a full re-emit when the underlying file "
        "hasn't changed (ANTS-1499 \"304 Not Modified\" pattern).";
    return memo.size();
}

// Offset of a tool's `["name"] = "<tool>"` registration in the source.
std::size_t namePos(const std::string &ci, const std::string &tool) {
    return ci.find("[\"name\"] = \"" + tool + "\"");
}

// The tool's `description` + `detail` slice: from its first
// `["description"]` after the name registration to the following
// `["selection_hint"]`. Empty string on any miss.
std::string descDetailBlock(const std::string &ci, const std::string &tool) {
    const auto np = namePos(ci, tool);
    if (np == std::string::npos) return {};
    const auto ds = ci.find("[\"description\"]", np);
    if (ds == std::string::npos) return {};
    const auto se = ci.find("[\"selection_hint\"]", ds);
    if (se == std::string::npos) return {};
    return ci.substr(ds, se - ds);
}

// Decoded UTF-8 byte length of the SHORT `description` literal (the first
// QStringLiteral(...) after the tool's name registration). Walks the
// adjacent-string-literal segments, collapsing `\x` escapes to one byte
// and counting raw UTF-8 bytes as-is, until the literal's closing `)`.
// Returns 0 on a miss.
std::size_t shortDescBytes(const std::string &ci, const std::string &tool,
                           char *firstChar = nullptr) {
    const auto np = namePos(ci, tool);
    if (np == std::string::npos) return 0;
    const auto ds = ci.find("[\"description\"]", np);
    if (ds == std::string::npos) return 0;
    auto i = ci.find("QStringLiteral(", ds);
    if (i == std::string::npos) return 0;
    i += std::string("QStringLiteral(").size();
    const std::size_t n = ci.size();
    std::size_t bytes = 0;
    bool gotFirst = false;
    while (i < n) {
        const char c = ci[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
        if (c == '"') {
            ++i;  // enter segment
            while (i < n && ci[i] != '"') {
                if (!gotFirst && firstChar) { *firstChar = ci[i]; }
                gotFirst = true;
                if (ci[i] == '\\' && i + 1 < n) { i += 2; }
                else { ++i; }
                ++bytes;
            }
            ++i;  // skip closing quote
            continue;
        }
        if (c == ')') break;  // end of the description literal
        ++i;  // stray (comma, etc.) — shouldn't occur inside the literal
    }
    return bytes;
}

// Read an anchor fixture: one token per line, `#` comments + blanks
// dropped, surrounding whitespace trimmed.
std::vector<std::string> readAnchors(const std::string &tool) {
    std::vector<std::string> out;
    const std::string path =
        std::string(ANTS_MCP_DETAIL_ANCHORS_DIR) + "/" + tool + ".txt";
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        std::size_t b = line.find_last_not_of(" \t\r");
        const std::string tok = line.substr(a, b - a + 1);
        if (tok.empty() || tok[0] == '#') continue;
        out.push_back(tok);
    }
    return out;
}

// ANTS-5152 — every `<var>["name"] = "<tool>";` registration line, in
// source order, with its offset: the same shape
// tools/check-readme-claims.sh counts as a registered tool.
std::vector<std::pair<std::string, std::size_t>> registrations(
    const std::string &ci) {
    std::vector<std::pair<std::string, std::size_t>> out;
    const std::string key = "[\"name\"] = \"";
    const auto isIdent = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    for (std::size_t p = ci.find(key); p != std::string::npos;
         p = ci.find(key, p + 1)) {
        std::size_t i = ci.rfind('\n', p);
        i = (i == std::string::npos) ? 0 : i + 1;
        while (i < p && (ci[i] == ' ' || ci[i] == '\t')) ++i;
        bool ok = i < p;
        for (std::size_t j = i; ok && j < p; ++j) ok = isIdent(ci[j]);
        const std::size_t ns = p + key.size();
        const std::size_t ne = ci.find('"', ns);
        if (!ok || ne == std::string::npos || ne == ns ||
            ci.compare(ne, 2, "\";") != 0)
            continue;
        const std::string name = ci.substr(ns, ne - ns);
        bool lower = true;
        for (char c : name) lower = lower && ((c >= 'a' && c <= 'z') || c == '_');
        if (lower) out.emplace_back(name, p);
    }
    return out;
}

// The names isEtagSupportedTool accepts: those descriptions carry the
// runtime "Etag tip:" memo on the wire.
std::vector<std::string> etagTools(const std::string &ci) {
    std::vector<std::string> out;
    const auto a = ci.find("bool ClaudeIntegration::isEtagSupportedTool(");
    if (a == std::string::npos) return out;
    const auto b = ci.find("\n}\n", a);
    const std::string body = ci.substr(a, b - a);
    const std::string lit = "QStringLiteral(\"";
    for (auto p = body.find(lit); p != std::string::npos;
         p = body.find(lit, p + 1)) {
        const auto s = p + lit.size();
        out.push_back(body.substr(s, body.find('"', s) - s));
    }
    return out;
}

}  // namespace

// INV-1 — the tools/list handler strips `detail` from the wire array,
// and every in-scope tool authored a `detail` literal to strip.
TEST(mcp_tool_detail_field, Inv1WireStripAndDetailAuthored) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::string sq = squashWhitespace(ci);
    expect(contains(sq, "t.remove(QStringLiteral(\"detail\"))"),
           "INV-1: tools/list handler removes `detail` from the wire array");
    for (const char *tool : kInScope) {
        const std::string blk = descDetailBlock(ci, tool);
        expect(!blk.empty(),
               (std::string("INV-1 setup: desc/detail block located for ") +
                   tool).c_str());
        expect(contains(blk, "[\"detail\"] = QStringLiteral("),
               (std::string("INV-1: ") + tool +
                   " authors a `detail` sibling literal").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — tool_info serves `detail` conditionally (present only for
// tools that authored one).
TEST(mcp_tool_detail_field, Inv2ToolInfoConditionalDetail) {
    expect_reset();
    const std::string sq = squashWhitespace(ciSource());
    expect(contains(sq, "if (match.contains(QStringLiteral(\"detail\"))) "
                        "env[\"detail\"] = "
                        "match.value(QStringLiteral(\"detail\"))"),
           "INV-2: tool_info sets env[\"detail\"] only when the matched "
           "descriptor carries a `detail` key");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — no documented refusal code / op / selector dropped: every
// anchor token still appears in the tool's desc+detail source slice.
TEST(mcp_tool_detail_field, Inv3NoDroppedAnchorTokens) {
    expect_reset();
    const std::string &ci = ciSource();
    for (const char *tool : kInScope) {
        const std::string blk = descDetailBlock(ci, tool);
        ASSERT_FALSE(blk.empty())
            << "INV-3 setup: desc/detail block missing for " << tool;
        const auto anchors = readAnchors(tool);
        ASSERT_FALSE(anchors.empty())
            << "INV-3 setup: anchor fixture empty/missing for " << tool;
        for (const auto &tok : anchors) {
            expect(contains(blk, tok),
                   (std::string("INV-3: ") + tool +
                       " desc/detail must retain anchor token `" + tok +
                       "`").c_str());
        }
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-4 — the wire strip appends the per-op pointer template.
TEST(mcp_tool_detail_field, Inv4WirePointerAppended) {
    expect_reset();
    expect(contains(ciSource(), "Full per-op detail via tool_info "),
           "INV-4: trimmed wire description gains the tool_info pointer");
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — reconstructed wire `description` (<=12 B [<kind>] prefix +
// short literal + Etag-tip memo where applicable + per-op pointer) is
// <= 800 B for every in-scope tool.
TEST(mcp_tool_detail_field, Inv5WireBudgetUnder800) {
    expect_reset();
    const std::string &ci = ciSource();
    const std::size_t kPrefixMax = 12;  // upper bound on "[<kind>] "
    for (const char *tool : kInScope) {
        const std::string name = tool;
        const std::size_t shortB = shortDescBytes(ci, name);
        ASSERT_GT(shortB, 0u)
            << "INV-5 setup: short description not parsed for " << tool;
        const std::string ptr =
            " Full per-op detail via tool_info {name:\"" + name + "\"}.";
        std::size_t wire = kPrefixMax + shortB + ptr.size();
        if (isEtagTool(name)) wire += etagTipBytes();
        expect(wire <= 800,
               (std::string("INV-5: ") + name +
                   " reconstructed wire description <= 800 B (got " +
                   std::to_string(wire) + ")").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — each short description does NOT itself start with `[`, so the
// runtime `[<kind>]`-prefix loop fires (and kindForName keeps bucketing).
TEST(mcp_tool_detail_field, Inv6ShortDescNotPrePrefixed) {
    expect_reset();
    const std::string &ci = ciSource();
    for (const char *tool : kInScope) {
        char first = '\0';
        const std::size_t shortB = shortDescBytes(ci, tool, &first);
        ASSERT_GT(shortB, 0u)
            << "INV-6 setup: short description not parsed for " << tool;
        expect(first != '[',
               (std::string("INV-6: ") + tool +
                   " short description must not begin with '[' (runtime "
                   "[<kind>] prefix loop prepends it)").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — snapshot precedes the strip, and the strip precedes the wire
// send: the snapshot keeps the clean short description + the detail
// sibling, and the pointer never leaks into what tool_info reads.
TEST(mcp_tool_detail_field, Inv7SnapshotBeforeStripBeforeSend) {
    expect_reset();
    const std::string &ci = ciSource();
    const auto snap = ci.find("m_lastToolsList = tools");
    const auto strip = ci.find("t.remove(QStringLiteral(\"detail\"))");
    const auto wantLite = ci.find("if (wantLite)");
    ASSERT_NE(snap, std::string::npos) << "INV-7: snapshot assignment missing";
    ASSERT_NE(strip, std::string::npos) << "INV-7: detail strip missing";
    ASSERT_NE(wantLite, std::string::npos) << "INV-7: wantLite branch missing";
    expect(snap < strip,
           "INV-7: m_lastToolsList snapshot must precede the detail strip");
    expect(strip < wantLite,
           "INV-7: detail strip must precede the lite/full wire send");
    EXPECT_EQ(0, expect_failures());
}

// INV-9 (ANTS-5152) — the INV-5 budget holds for EVERY registered tool,
// not only the seven above: prefix + short literal + the pointer when a
// `detail` is authored + the Etag tip on etag-supported tools <= 800 B.
TEST(mcp_tool_detail_field, Inv9EveryToolWireBudgetUnder800) {
    expect_reset();
    const std::string &ci = ciSource();
    const auto regs = registrations(ci);
    ASSERT_GT(regs.size(), kInScope.size())
        << "INV-9 setup: tool registrations not found";
    const auto etag = etagTools(ci);
    ASSERT_FALSE(etag.empty()) << "INV-9 setup: isEtagSupportedTool not parsed";
    const std::size_t kPrefixMax = 12;
    for (std::size_t k = 0; k < regs.size(); ++k) {
        const std::string &name = regs[k].first;
        const std::size_t start = regs[k].second;
        const std::size_t end =
            k + 1 < regs.size() ? regs[k + 1].second : ci.size();
        const std::string block = ci.substr(start, end - start);
        const auto ds = block.find("[\"description\"]");
        expect(ds != std::string::npos,
               ("INV-9: " + name + " has a description in its block").c_str());
        if (ds == std::string::npos) continue;
        std::size_t shortB = shortDescBytes(ci, name);
        // A plain `"..."` literal (no QStringLiteral) is measured as written.
        const auto lit = block.find_first_not_of(
            " =", ds + std::string("[\"description\"]").size());
        if (lit != std::string::npos && block[lit] == '"')
            shortB = block.find('"', lit + 1) - lit - 1;
        expect(shortB > 0,
               ("INV-9: " + name + " short description parsed").c_str());
        std::size_t wire = kPrefixMax + shortB;
        if (contains(block, "[\"detail\"] = QStringLiteral("))
            wire += (" Full per-op detail via tool_info {name:\"" + name +
                     "\"}.").size();
        for (const auto &e : etag)
            if (e == name) wire += etagTipBytes();
        expect(wire <= 800,
               ("INV-9: " + name + " reconstructed wire description <= 800 B" +
                " (got " + std::to_string(wire) + ")").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}
