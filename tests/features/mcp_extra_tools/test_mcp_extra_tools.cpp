// Source-grep harness for ANTS-1244 — pins the wiring contract for
// the 3 new MCP tools (roadmap_query, tab_list, get_text) added on
// top of the 6 existing tools. See spec.md.
//
// Exit 0 = all 7 invariants hold.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

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

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

static int runMain() {
    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);

    int failures = 0;
    auto inv = [&](int n, bool ok, const char *why) {
        if (!ok) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "INV-%d", n);
            failures += fail(buf, why);
        }
    };

    // INV-1 — tools/list registers the 3 new tool names.
    inv(1, contains(ciCpp, "\"roadmap_query\""),
        "tools/list missing \"roadmap_query\" name registration");
    inv(1, contains(ciCpp, "\"tab_list\""),
        "tools/list missing \"tab_list\" name registration");
    inv(1, contains(ciCpp, "\"get_text\""),
        "tools/list missing \"get_text\" name registration");

    // INV-2 — tools/call dispatcher has new else-if clauses for each
    // new tool. Allow either order; check that each tool name appears
    // in a `toolName == "..."` comparison.
    std::regex routeRq(
        R"(toolName\s*==\s*"roadmap_query")");
    std::regex routeTl(
        R"(toolName\s*==\s*"tab_list")");
    std::regex routeGt(
        R"(toolName\s*==\s*"get_text")");
    inv(2, std::regex_search(ciCpp, routeRq),
        "tools/call missing `toolName == \"roadmap_query\"` clause");
    inv(2, std::regex_search(ciCpp, routeTl),
        "tools/call missing `toolName == \"tab_list\"` clause");
    inv(2, std::regex_search(ciCpp, routeGt),
        "tools/call missing `toolName == \"get_text\"` clause");

    // INV-3 — claudeintegration.h declares the 3 new setters with the
    // expected `std::function` signatures.
    // ANTS-1247: setRoadmapQueryProvider widened to accept a `const
    // QString&` status filter.
    inv(3, contains(ciHdr,
            "setRoadmapQueryProvider(std::function<QString(const QString&)>"),
        "setRoadmapQueryProvider not declared with std::function<QString(const QString&)> (ANTS-1247 widen)");
    inv(3, contains(ciHdr,
            "setTabListProvider(std::function<QString()>"),
        "setTabListProvider not declared with std::function<QString()>");
    inv(3, contains(ciHdr,
            "setGetTextProvider(std::function<QString(int"),
        "setGetTextProvider not declared with std::function<QString(int,…)> shape");

    // INV-4 — matching private member variables exist.
    inv(4, contains(ciHdr, "m_roadmapQueryProvider"),
        "claudeintegration.h missing m_roadmapQueryProvider member");
    inv(4, contains(ciHdr, "m_tabListProvider"),
        "claudeintegration.h missing m_tabListProvider member");
    inv(4, contains(ciHdr, "m_getTextProvider"),
        "claudeintegration.h missing m_getTextProvider member");

    // INV-5 — remotecontrol.h promotes the 3 cmd handlers to public.
    // We need to find each handler's declaration AFTER a `public:` and
    // BEFORE the next `private:` (if any). Simplest invariant: the
    // method declarations must appear inside a public section.
    //
    // The header has at most one `private:` section before the methods'
    // current home; check that each method appears AFTER a `public:` and
    // either (a) before any `private:` or (b) the method appears in a
    // public block (we re-scan for the closest preceding access label).
    auto isInPublicSection = [&](const std::string &needle) {
        size_t pos = rcHdr.find(needle);
        if (pos == std::string::npos) return false;
        // Find the closest preceding `public:` or `private:` label.
        size_t publicPos  = rcHdr.rfind("public:", pos);
        size_t privatePos = rcHdr.rfind("private:", pos);
        size_t protectedPos = rcHdr.rfind("protected:", pos);
        size_t closest = std::string::npos;
        auto setIfCloser = [&](size_t p) {
            if (p == std::string::npos) return;
            if (closest == std::string::npos || p > closest) closest = p;
        };
        setIfCloser(publicPos);
        setIfCloser(privatePos);
        setIfCloser(protectedPos);
        if (closest == std::string::npos) return false;
        // Compare against the public label position
        return closest == publicPos;
    };
    inv(5, isInPublicSection("cmdRoadmapQuery"),
        "remotecontrol.h: cmdRoadmapQuery not in a public: section");
    inv(5, isInPublicSection("cmdTabList"),
        "remotecontrol.h: cmdTabList not in a public: section");
    inv(5, isInPublicSection("cmdGetText"),
        "remotecontrol.h: cmdGetText not in a public: section");

    // INV-6 — setupClaudeMcpProviders calls each of the 3 setters and
    // the surrounding lambdas refer to m_remoteControl. We don't pin
    // the exact lambda body — just that the setter calls + the
    // m_remoteControl token both appear in setupClaudeMcpProviders.
    size_t setupPos = mwCpp.find("setupClaudeMcpProviders()");
    inv(6, setupPos != std::string::npos,
        "mainwindow.cpp: setupClaudeMcpProviders() definition not found");
    if (setupPos != std::string::npos) {
        // Bound the search to the function body — heuristic: from
        // the opening brace to the matching close. Cap at 8 KB to
        // keep the search local.
        size_t braceOpen = mwCpp.find('{', setupPos);
        std::string body = (braceOpen == std::string::npos)
            ? std::string()
            : mwCpp.substr(braceOpen, 8 * 1024);
        inv(6, contains(body, "setRoadmapQueryProvider("),
            "setupClaudeMcpProviders does not call setRoadmapQueryProvider");
        inv(6, contains(body, "setTabListProvider("),
            "setupClaudeMcpProviders does not call setTabListProvider");
        inv(6, contains(body, "setGetTextProvider("),
            "setupClaudeMcpProviders does not call setGetTextProvider");
        inv(6, contains(body, "m_remoteControl"),
            "setupClaudeMcpProviders lambdas do not reference m_remoteControl");
    }

    // INV-7 — the get_text dispatch case uses isDouble() to
    // distinguish "argument absent" from "argument present and 0",
    // mirroring the IPC verb (remotecontrol.cpp:347) and satisfying
    // spec INV-9.
    //
    // We bound the search to the few hundred bytes after the
    // `toolName == "get_text"` line — the case body is short.
    size_t gtPos = ciCpp.find("toolName == \"get_text\"");
    if (gtPos == std::string::npos)
        gtPos = ciCpp.find("toolName==\"get_text\"");
    if (gtPos == std::string::npos) {
        ++failures;
        std::fprintf(stderr,
            "[INV-7] FAIL: get_text dispatch clause not findable for isDouble() check\n");
    } else {
        std::string block = ciCpp.substr(gtPos, 800);
        inv(7, contains(block, "isDouble()"),
            "get_text dispatcher does not use isDouble() to gate "
            "tab/lines extraction (spec INV-9)");
    }

    if (failures > 0) {
        std::fprintf(stderr,
            "\n%d invariant(s) failed — see ANTS-1244 spec for context\n",
            failures);
        return 1;
    }
    std::printf("OK: 7/7 MCP extra-tool wiring invariants present\n");
    return 0;
}

TEST(McpExtraTools, Main) {
    if (runMain() != 0) FAIL();
}
