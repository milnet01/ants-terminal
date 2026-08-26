// ANTS-1642 — feature-conformance test for MCP dispatch-forward
// completeness. Walks every prop declared in a tool's
// `claudeintegration.cpp` registration block and asserts the matching
// `mainwindow.cpp` `registerToolProvider` lambda reads it via
// `args.value("X")` — unless the lambda hands `args` directly to a
// cmd handler (INV-2) or the prop is on the transport-handled / known-
// good allow-list (INV-3/4).
//
// Catches the bug shape that shipped three times unnoticed:
// ANTS-1437 (`mode`), ANTS-1398 (`include_section_headers`),
// ANTS-1586 (`include_body`).

#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Extract the set of property names declared inside a tool's
// registration block (between `t["name"] = "X"` and the matching
// `tools.append(t)`). Each declaration looks like:
//   props["foo"] = fooProp;
// We scan brace-balanced from the name match to the next
// `tools.append(t)` and collect every `props["IDENT"]` token.
std::set<std::string> collectProps(const std::string &src,
                                   const std::string &toolName) {
    std::set<std::string> out;
    const std::string nameAnchor = "t[\"name\"] = \"" + toolName + "\"";
    const std::size_t startPos = src.find(nameAnchor);
    if (startPos == std::string::npos) return out;
    const std::size_t endPos = src.find("tools.append(t)", startPos);
    if (endPos == std::string::npos) return out;
    const std::string block = src.substr(startPos, endPos - startPos);

    // Scan for every `props["IDENT"]` token. Hand-rolled rather than
    // regex to keep the test free of <regex>.
    const std::string needle = "props[\"";
    std::size_t pos = 0;
    while ((pos = block.find(needle, pos)) != std::string::npos) {
        const std::size_t qStart = pos + needle.size();
        const std::size_t qEnd   = block.find('"', qStart);
        if (qEnd == std::string::npos) break;
        out.insert(block.substr(qStart, qEnd - qStart));
        pos = qEnd + 1;
    }
    return out;
}

// Extract the lambda body for `registerToolProvider("TOOL", …)` from
// mainwindow.cpp. Returns empty string if the tool isn't registered.
// Uses balanced-paren scan from the `(` after `registerToolProvider`
// out to the matching `);` — careful to step over brace-nested code
// inside the lambda body itself.
std::string extractLambdaBody(const std::string &src,
                              const std::string &toolName) {
    const std::string anchor =
        "registerToolProvider(\"" + toolName + "\"";
    const std::size_t startPos = src.find(anchor);
    if (startPos == std::string::npos) return {};
    // Walk forward from the opening `(` of registerToolProvider.
    const std::size_t openParen = src.find('(', startPos);
    if (openParen == std::string::npos) return {};

    int parenDepth = 0;
    bool inLineComment  = false;
    bool inBlockComment = false;
    bool inStr  = false;
    bool inChar = false;
    bool inRawStr = false;
    char prev = '\0';
    std::size_t i = openParen;
    for (; i < src.size(); ++i) {
        const char c = src[i];
        // Comment handling.
        if (inLineComment) {
            if (c == '\n') inLineComment = false;
            prev = c;
            continue;
        }
        if (inBlockComment) {
            if (prev == '*' && c == '/') inBlockComment = false;
            prev = c;
            continue;
        }
        // String / char literal handling — toggle off escape.
        if (inStr) {
            if (c == '"' && prev != '\\') inStr = false;
            prev = c;
            continue;
        }
        if (inChar) {
            if (c == '\'' && prev != '\\') inChar = false;
            prev = c;
            continue;
        }
        // Start of comment / literal.
        if (prev == '/' && c == '/') { inLineComment = true; prev = c; continue; }
        if (prev == '/' && c == '*') { inBlockComment = true; prev = c; continue; }
        if (c == '"')  { inStr = true;  prev = c; continue; }
        if (c == '\'') { inChar = true; prev = c; continue; }
        // Paren balance — the meaningful state.
        if (c == '(') ++parenDepth;
        else if (c == ')') {
            --parenDepth;
            if (parenDepth == 0) {
                // Body spans from openParen+1 through i-1.
                return src.substr(openParen + 1, i - openParen - 1);
            }
        }
        prev = c;
        (void)inRawStr;  // unused — kept for future hardening
    }
    return {};
}

// Lambda is "pass-through" iff it hands `args` directly to a
// cmd…(args) invocation. The IPC handler reads the input object
// directly, so per-arg forwarding inside the lambda is redundant.
bool isPassThroughLambda(const std::string &body) {
    // Common shapes:
    //   m_remoteControl->cmdRoadmapLog(args)
    //   cmdXxx(args)
    //   cmdTokenUsage(args, m_claudeIntegration)   ← multi-arg
    //   AuditEngine::run(args)
    // A lambda is pass-through when it hands `args` (whole object,
    // not a single field) to a downstream callable — `args.` would
    // be a field read, so we require `args` followed by a non-`.`
    // character that signals continuation as a function argument:
    // `)` (sole arg), `,` (one of several args), or `;` (rare).
    const std::string forms[] = {
        "(args)", "( args )", "(args,", "( args,",
        " args)", ", args)", ", args,",
        // ANTS-1782 — the rcDelegate(&RemoteControl::cmd*) factory
        // builds a handler that forwards the whole `args` object to the
        // cmd verb, so a registration handed an rcDelegate(...) is
        // pass-through by construction (same as `cmd(args)` inline).
        "rcDelegate(",
    };
    for (const auto &f : forms) {
        if (contains(body, f)) return true;
    }
    return false;
}

// Collapse all runs of ASCII whitespace (space / tab / newline / CR)
// to a single space. Used to make the args.value / args.contains
// matcher tolerant of the codebase's multi-line argument-formatting
// style:
//
//     args.value(
//         QStringLiteral("caller_cwd")).toString()
//
// — a line-wrapped read which a `contains(needle)` over the raw body
// would never match.
std::string collapseWhitespace(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (char c : s) {
        const bool isWs = (c == ' ' || c == '\t' ||
                           c == '\n' || c == '\r');
        if (isWs) {
            if (!prevSpace) out.push_back(' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    return out;
}

bool readsArgValue(const std::string &collapsedBody,
                   const std::string &prop) {
    // Windowed match: find every `args.value(` or `args.contains(`
    // call site in the collapsed body and check whether the literal
    // `"<prop>"` appears within the next ~60 chars. Covers both
    // direct (`args.value("X")`) and QStringLiteral-wrapped
    // (`args.value(QStringLiteral("X"))`) forms across any
    // line-wrapping the formatter chose, without enumerating each
    // permutation of inner whitespace.
    const std::string needle = "\"" + prop + "\"";
    const std::string anchors[] = {"args.value(", "args.contains("};
    for (const auto &anchor : anchors) {
        std::size_t pos = 0;
        while ((pos = collapsedBody.find(anchor, pos)) != std::string::npos) {
            const std::size_t windowStart = pos + anchor.size();
            const std::size_t windowEnd =
                std::min(collapsedBody.size(), windowStart + 60);
            if (collapsedBody.substr(windowStart,
                                     windowEnd - windowStart)
                    .find(needle) != std::string::npos) {
                return true;
            }
            pos += anchor.size();
        }
    }
    return false;
}

// Transport-handled props — consumed by ClaudeIntegration before the
// lambda runs.
bool isTransportHandled(const std::string &prop) {
    return prop == "etag_match";
}

// (tool, prop) pairs the test deliberately ignores. Empty as of
// ANTS-1642 — kept as an escape hatch. Each future entry must carry
// a `Reason:` comment explaining why the prop is intentionally not
// forwarded.
bool isAllowListed(const std::string & /*tool*/,
                   const std::string & /*prop*/) {
    return false;
}

}  // namespace

// INV-1 — every schema-advertised prop is read by the dispatch
// lambda (with INV-2/3/4 exemptions). One test, one failure surface,
// all gaps reported in a single run so a fix lands in one pass.
TEST(McpDispatchForwardCompleteness, EveryPropReadInLambda) {
    const std::string ci  = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mw  = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(ci.empty()) << "could not read claudeintegration.cpp";
    ASSERT_FALSE(mw.empty()) << "could not read mainwindow.cpp";

    // Enumerate every registered tool by scanning mainwindow.cpp.
    // Each registration: `registerToolProvider("X", …)`.
    std::vector<std::string> toolNames;
    {
        const std::string anchor = "registerToolProvider(\"";
        std::size_t pos = 0;
        while ((pos = mw.find(anchor, pos)) != std::string::npos) {
            const std::size_t qStart = pos + anchor.size();
            const std::size_t qEnd   = mw.find('"', qStart);
            if (qEnd == std::string::npos) break;
            toolNames.push_back(mw.substr(qStart, qEnd - qStart));
            pos = qEnd + 1;
        }
    }
    ASSERT_FALSE(toolNames.empty())
        << "no registerToolProvider calls found — anchor drift?";

    // Sanity: > 30 tools registered as of writing. Guards the
    // anchor pattern against a future rename that would silently
    // make the test a no-op.
    EXPECT_GT(toolNames.size(), std::size_t{30})
        << "expected > 30 tools; anchor may have drifted";

    std::vector<std::pair<std::string, std::string>> misses;

    for (const auto &tool : toolNames) {
        const auto props = collectProps(ci, tool);
        if (props.empty()) {
            // Either a tool with no props (e.g. tab_list takes only
            // etag_match) or a tool whose registration block uses a
            // non-canonical shape. Skip — INV-1 has nothing to
            // assert.
            continue;
        }
        const std::string body = extractLambdaBody(mw, tool);
        if (body.empty()) {
            ADD_FAILURE() << "tool \"" << tool
                          << "\" registers in claudeintegration.cpp "
                             "but no lambda body found in mainwindow.cpp";
            continue;
        }
        // INV-2 — pass-through lambdas implicitly forward every prop.
        if (isPassThroughLambda(body)) continue;

        // Pre-collapse the body so the args.value matcher tolerates
        // multi-line argument formatting (the codebase's
        // claudeintegration / mainwindow style wraps long calls).
        const std::string collapsedBody = collapseWhitespace(body);

        for (const auto &prop : props) {
            // INV-3 — transport-handled props are exempt.
            if (isTransportHandled(prop)) continue;
            // INV-4 — explicit allow-list.
            if (isAllowListed(tool, prop)) continue;
            // INV-1 — must be read in the lambda body.
            if (!readsArgValue(collapsedBody, prop)) {
                misses.emplace_back(tool, prop);
            }
        }
    }

    if (!misses.empty()) {
        std::ostringstream oss;
        oss << "\nANTS-1642 INV-1: " << misses.size()
            << " schema prop(s) silently dropped by their dispatch "
               "lambda (catches the ANTS-1437/1398/1586 bug shape):\n";
        for (const auto &[tool, prop] : misses) {
            oss << "  - " << tool << " :: \"" << prop << "\"\n";
        }
        oss << "Fix: either add `args.value(\"PROP\")` + `req[\"PROP\"] = …` "
               "to the lambda, or change the lambda to pass `args` "
               "directly to the cmd handler.";
        FAIL() << oss.str();
    }
}
