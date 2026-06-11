// ANTS-1567 — feature-conformance test for the `kindForName` prefix
// mapping that ANTS-1518 attaches to every tool/list description.
// Source-scrape against claudeintegration.cpp and mainwindow.cpp:
// asserts the two label renames and that no registered tool falls
// into the `"other"` bucket.

#include "../../_support/expect.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

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
