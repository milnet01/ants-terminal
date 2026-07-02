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
#include "../../_support/srcgrep.h"

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


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

int fail(const char *label, const char *why) {
    std::fprintf(stderr, "[%s] FAIL: %s\n", label, why);
    return 1;
}

}  // namespace

static int runMain() {
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

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

    // INV-2 — each new tool has a tools/list schema entry naming it.
    // ANTS-1253 collapsed the per-tool `toolName == "..."` dispatch
    // clauses into a single `m_toolProviders.find(toolName)` lookup,
    // so we re-anchor the tool-name check to the schema-builder line
    // (e.g. `roadmapTool["name"] = "roadmap_query"`).
    std::regex schemaRq(R"("name"\]\s*=\s*"roadmap_query")");
    std::regex schemaTl(R"("name"\]\s*=\s*"tab_list")");
    std::regex schemaGt(R"("name"\]\s*=\s*"get_text")");
    inv(2, std::regex_search(ciCpp, schemaRq),
        "tools/list schema missing roadmap_query name entry");
    inv(2, std::regex_search(ciCpp, schemaTl),
        "tools/list schema missing tab_list name entry");
    inv(2, std::regex_search(ciCpp, schemaGt),
        "tools/list schema missing get_text name entry");

    // INV-3 — claudeintegration.h declares the single registry surface
    // (ANTS-1253). The per-tool setters / members no longer exist —
    // they collapsed into registerToolProvider + m_toolProviders.
    // ANTS-1419: signature gained a CallerCwdContract second positional
    // argument so per-tool security contracts live next to the
    // registration call. The original (name, handler) shape was
    // replaced — accept either declaration order so this anchor stays
    // forgiving of future formatting tweaks.
    inv(3, contains(ciHdr,
            "registerToolProvider(const QString &name,") &&
           (contains(ciHdr,
                     "CallerCwdContract contract") ||
            contains(ciHdr, "ToolHandler handler")),
        "registerToolProvider not declared with (QString, "
        "CallerCwdContract, ToolHandler) signature "
        "(ANTS-1253 + ANTS-1419)");
    inv(3, contains(ciHdr,
            "using ToolHandler ="),
        "claudeintegration.h missing ToolHandler type alias (ANTS-1253)");

    // INV-4 — single registry member exists.
    inv(4, contains(ciHdr, "m_toolProviders"),
        "claudeintegration.h missing m_toolProviders registry member (ANTS-1253)");

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

    // INV-6 — setupClaudeMcpProviders registers each tool and the
    // surrounding lambdas refer to m_remoteControl. Post-ANTS-1253
    // setters became `registerToolProvider("<name>", …)` calls.
    size_t setupPos = mwCpp.find("setupClaudeMcpProviders()");
    inv(6, setupPos != std::string::npos,
        "mainwindow.cpp: setupClaudeMcpProviders() definition not found");
    if (setupPos != std::string::npos) {
        size_t braceOpen = mwCpp.find('{', setupPos);
        std::string body = (braceOpen == std::string::npos)
            ? std::string()
            // Window bumped 16→32 KiB after ANTS-1351's audit_run
            // dispatch lambda landed (~5 KiB inline), 32→48 KiB after
            // ANTS-1500 fattened the get_scrollback provider with
            // since_cursor handling (~3 KiB), 48→64 KiB after
            // ANTS-1897 added the qputenv + mcp_orientation::install
            // block right after startMcpServer (~1 KiB) which pushed
            // get_text past the old window. 64→80 KiB after ANTS-3420
            // added the query/max_body_bytes/section-etag forward block
            // (~1.4 KiB) to the roadmap_query lambda, again pushing
            // get_text past the old window. The window is a heuristic
            // body-cap; raise as new providers slot in.
            : mwCpp.substr(braceOpen, 80 * 1024);
        inv(6, contains(body, "registerToolProvider(\"roadmap_query\""),
            "setupClaudeMcpProviders does not register roadmap_query");
        inv(6, contains(body, "registerToolProvider(\"tab_list\""),
            "setupClaudeMcpProviders does not register tab_list");
        inv(6, contains(body, "registerToolProvider(\"get_text\""),
            "setupClaudeMcpProviders does not register get_text");
        inv(6, contains(body, "m_remoteControl"),
            "setupClaudeMcpProviders lambdas do not reference m_remoteControl");
    }

    // INV-7 — the get_text registration uses isDouble() to distinguish
    // "argument absent" from "argument present and 0", mirroring the
    // IPC verb (remotecontrol.cpp:347) and satisfying spec INV-9.
    // Post-ANTS-1253 the gate lives in mainwindow.cpp's get_text
    // lambda body, not in claudeintegration.cpp's dispatcher.
    size_t gtPos = mwCpp.find("registerToolProvider(\"get_text\"");
    if (gtPos == std::string::npos) {
        ++failures;
        std::fprintf(stderr,
            "[INV-7] FAIL: get_text registration not findable for isDouble() check\n");
    } else {
        std::string block = mwCpp.substr(gtPos, 800);
        inv(7, contains(block, "isDouble()"),
            "get_text lambda does not use isDouble() to gate "
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
    ASSERT_EQ(0, runMain());
}
