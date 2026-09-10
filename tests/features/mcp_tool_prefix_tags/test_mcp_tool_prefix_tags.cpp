// ANTS-1567 — feature-conformance test for the `kindForName` prefix
// mapping that ANTS-1518 attaches to every tool/list description.
// Source-scrape against claudeintegration.cpp and mainwindow.cpp:
// asserts the two label renames and that no registered tool falls
// into the `"other"` bucket.
//
// ANTS-3645 part (b) adds INV-4: the complementary failure mode, where a
// tool's own description already begins with `[` and silently suppresses
// the prefix loop's tag instead of falling into `other`. See spec.md.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Extract the body of the `kindForName` lambda by string-slicing
// between its opening `[](const QString &name) -> QString {` and the
// matching `};` at the end of the block.
std::string extractKindForNameBody(const std::string &src) {
    const std::string marker = "auto kindForName = [](const QString &name)";
    const size_t open = src.find(marker);
    if (open == std::string::npos) return std::string();
    const size_t brace = src.find('{', open);
    if (brace == std::string::npos) return std::string();
    // Walk braces to find the matching close.
    int depth = 1;
    size_t i = brace + 1;
    while (i < src.size() && depth > 0) {
        if (src[i] == '{') ++depth;
        else if (src[i] == '}') --depth;
        ++i;
    }
    if (depth != 0) return std::string();
    return src.substr(brace, i - brace);
}

// Walk `registerToolProvider("<name>"` calls and collect every name
// passed as the first argument. Returns the set of tool names
// registered in MainWindow.
std::set<std::string> collectRegisteredToolNames(const std::string &src) {
    std::set<std::string> names;
    const std::string marker = "registerToolProvider(\"";
    size_t p = 0;
    while ((p = src.find(marker, p)) != std::string::npos) {
        p += marker.size();
        const size_t q = src.find('"', p);
        if (q == std::string::npos) break;
        names.insert(src.substr(p, q - p));
        p = q + 1;
    }
    return names;
}

// Every `<var>["name"] = "<value>";` assignment in claudeintegration.cpp,
// skipping the one whose variable is `serverInfo` (the MCP server's own
// self-descriptor, not a tool). Anchored on the NAME VALUE rather than on
// the assigning variable's spelling — descriptor locals are one-off per
// tool (`wsTool`, `sessionTool`, plain `t`, ...), so no single variable
// name covers every registration, and the value string is the only
// constant across all of them.
//
// Deliberately does NOT reuse `collectRegisteredToolNames` above: that
// walks mainwindow.cpp's `registerToolProvider(...)` calls, which misses
// `tool_info` and `get_session_info` — both built inline in the tools/list
// handler rather than registered through a provider. INV-4 below needs
// every tool actually written to the wire, so it scans
// claudeintegration.cpp itself.
std::vector<std::string> collectAllToolNamesInClaudeIntegration(
    const std::string &ci) {
    std::vector<std::string> names;
    const std::string marker = "[\"name\"] = \"";
    std::size_t p = 0;
    while ((p = ci.find(marker, p)) != std::string::npos) {
        std::size_t idStart = p;
        while (idStart > 0 &&
               (std::isalnum(static_cast<unsigned char>(ci[idStart - 1])) ||
                ci[idStart - 1] == '_')) {
            --idStart;
        }
        const std::string var = ci.substr(idStart, p - idStart);
        const std::size_t valStart = p + marker.size();
        const std::size_t valEnd = ci.find('"', valStart);
        if (valEnd == std::string::npos) break;
        if (var != "serverInfo") {
            names.push_back(ci.substr(valStart, valEnd - valStart));
        }
        p = valEnd + 1;
    }
    return names;
}

// The decoded first character of a tool's short `description` literal.
// Handles both shapes seen in claudeintegration.cpp: the common
// `QStringLiteral("...")` (including adjacent-segment continuations
// across lines) and a bare `"...";` literal — the one exception,
// `get_session_info`. Returns false, rather than a guessed character,
// when the assignment matches neither shape: a description authored a
// third way must fail this loudly, not get silently attributed to
// whatever QStringLiteral happens to follow it (the trap a naive
// find("QStringLiteral(", ...) falls into on `get_session_info`, whose
// plain literal has no QStringLiteral for the scan to stop at — it would
// walk on into the next tool's `selection_hint`).
bool descriptionFirstChar(const std::string &ci, const std::string &tool,
                           char *out) {
    const std::string anchor = "[\"name\"] = \"" + tool + "\"";
    const std::size_t np = ci.find(anchor);
    if (np == std::string::npos) return false;
    const std::size_t ds = ci.find("[\"description\"]", np);
    if (ds == std::string::npos) return false;
    // A description past the NEXT tool's name belongs to that tool. Reading
    // it would attribute another tool's text to this one, silently.
    const std::size_t nextName = ci.find("[\"name\"] = \"", np + anchor.size());
    if (nextName != std::string::npos && nextName < ds) return false;
    const std::size_t eq = ci.find('=', ds);
    if (eq == std::string::npos) return false;
    std::size_t i = eq + 1;
    while (i < ci.size() &&
           std::isspace(static_cast<unsigned char>(ci[i]))) {
        ++i;
    }
    const std::string qsl = "QStringLiteral(";
    if (ci.compare(i, qsl.size(), qsl) == 0) {
        i += qsl.size();
        while (i < ci.size() &&
               std::isspace(static_cast<unsigned char>(ci[i]))) {
            ++i;
        }
        if (i >= ci.size() || ci[i] != '"') return false;
        ++i;
        if (i >= ci.size()) return false;
        *out = (ci[i] == '\\' && i + 1 < ci.size()) ? ci[i + 1] : ci[i];
        return true;
    }
    if (ci[i] == '"') {
        ++i;
        if (i >= ci.size()) return false;
        *out = (ci[i] == '\\' && i + 1 < ci.size()) ? ci[i + 1] : ci[i];
        return true;
    }
    return false;  // neither recognised literal shape
}

}  // namespace

// INV-1 — session_memory resolves to mcp-state, not memory.
TEST(mcp_tool_prefix_tags, Inv1SessionMemoryIsMcpState) {
    expect_reset();
    const std::string body =
        extractKindForNameBody(ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    expect(!body.empty(),
           "INV-1 setup: kindForName lambda located in source");
    expect(contains(body, "\"session_memory\"") &&
           contains(body, "QStringLiteral(\"mcp-state\")"),
           "INV-1: session_memory branch returns mcp-state "
           "(renamed from memory per ANTS-1567)");
    expect(!contains(body, "QStringLiteral(\"memory\")"),
           "INV-1 negative: old `memory` literal removed from the "
           "kindForName mapping");
    EXPECT_EQ(0, expect_failures());
}

// INV-2 — caller_cwd_info resolves to meta, not terminal.
TEST(mcp_tool_prefix_tags, Inv2CallerCwdInfoIsMeta) {
    expect_reset();
    const std::string body =
        extractKindForNameBody(ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH));
    expect(!body.empty(),
           "INV-2 setup: kindForName lambda located in source");
    // The meta-bucket block must include caller_cwd_info.
    const size_t metaPos = body.find("QStringLiteral(\"meta\")");
    expect(metaPos != std::string::npos,
           "INV-2 setup: meta-bucket return present");
    // Look for caller_cwd_info inside the preceding ~600 chars (the
    // if-chain branch that returns "meta").
    const size_t windowStart =
        metaPos > 600 ? metaPos - 600 : 0;
    const std::string metaWindow =
        body.substr(windowStart, metaPos - windowStart);
    expect(contains(metaWindow, "\"caller_cwd_info\""),
           "INV-2: caller_cwd_info is bucketed under meta "
           "(moved from terminal per ANTS-1567)");
    EXPECT_EQ(0, expect_failures());
}

// INV-3 — every tool registered via registerToolProvider in
// mainwindow.cpp has a matching branch in kindForName. Walks both
// files; fails loudly if a new tool is registered without updating
// the bucket mapping.
TEST(mcp_tool_prefix_tags, Inv3EveryRegisteredToolHasBucket) {
    expect_reset();
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw =
        ants_test::slurpFile(SRC_MAINWINDOW_PATH);
    const std::string body = extractKindForNameBody(ci);
    expect(!body.empty(),
           "INV-3 setup: kindForName lambda located");
    const std::set<std::string> registered =
        collectRegisteredToolNames(mw);
    expect(registered.size() >= 30,
           "INV-3 setup: at least 30 tool registrations seen "
           "(sanity check)");
    for (const std::string &name : registered) {
        // The bucket mapping uses either an exact `QLatin1String("<name>")`
        // match OR a `startsWith(QStringLiteral("<prefix_>"))` family
        // match. Accept either form.
        const std::string quoted = "\"" + name + "\"";
        if (contains(body, quoted)) continue;
        // Family-prefix paths — strip after the last `_` and look
        // for `startsWith(QStringLiteral("<prefix>_"))`.
        const size_t us = name.rfind('_');
        bool familyMatch = false;
        if (us != std::string::npos) {
            // Walk every prefix from the full leading slug down to
            // the first two segments (e.g.
            // "test_audit_partition" → "test_audit_").
            std::string head;
            size_t cursor = 0;
            while (cursor < name.size()) {
                const size_t next = name.find('_', cursor);
                if (next == std::string::npos) break;
                head = name.substr(0, next + 1);
                const std::string probe =
                    "startsWith(QStringLiteral(\"" + head + "\"))";
                if (contains(body, probe)) {
                    familyMatch = true;
                    break;
                }
                cursor = next + 1;
            }
        }
        if (!familyMatch) {
            std::fprintf(stderr,
                "INV-3: registered tool %s not covered by "
                "kindForName — would fall into [other]\n",
                name.c_str());
            ADD_FAILURE() << "tool " << name
                          << " not bucketed in kindForName";
        }
    }
}

// INV-4 — no registered MCP tool's short `description` literal begins
// with `[`. The tools/list prefix loop only prepends `[<kind>] ` when
// `!desc.startsWith('[')` (idempotent, so a repeated tools/list call
// cannot double it) — so a description an author writes with a leading
// bracket silently SUPPRESSES its own kind tag instead of doubling it:
// the guard sees the bracket, skips, and the tool lists under whatever
// text the author wrote instead of the tag `kindForName` assigned it.
// ANTS-3645 part (b).
TEST(mcp_tool_prefix_tags, Inv4NoDescriptionPreBracketed) {
    expect_reset();
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::vector<std::string> names =
        collectAllToolNamesInClaudeIntegration(ci);
    expect(names.size() >= 90,
           "INV-4 setup: at least 90 tool name registrations seen "
           "(sanity check)");
    for (const std::string &name : names) {
        char first = '\0';
        const bool ok = descriptionFirstChar(ci, name, &first);
        expect(ok,
               (std::string("INV-4 setup: description literal not "
                   "recognised (neither QStringLiteral(...) nor a bare "
                   "literal) for ") + name).c_str());
        if (!ok) continue;
        expect(first != '[',
               (std::string("INV-4: ") + name +
                   " description must not begin with '[' — it would "
                   "suppress the runtime [<kind>] prefix instead of "
                   "letting it apply").c_str());
    }
    EXPECT_EQ(0, expect_failures());
}
