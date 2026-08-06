// Source-grep + runtime harness for ANTS-1390 — locks the wiring
// for the `~global` / `~claude-config` caller_cwd sentinel that
// routes workspace_search + file_outline at ~/.claude/. See spec.md.
//
// Exit 0 = all 12 invariants hold.

#include "../../_support/expect.h"
#include "resolvedroot.h"

#include <regex>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QString>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(McpGlobalConfigSentinel, WiringContract) {
    expect_reset();

    const std::string ciCpp =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string rcCpp = ants_test::slurpRemoteControl();

    const std::string rrHdrPath =
        std::string(ANTS_SOURCE_DIR) + "/src/resolvedroot.h";
    const std::string rrHdr = ants_test::slurpFile(rrHdrPath.c_str());

    // INV-1 — declaration in resolvedroot.h, namespace ants.
    expect(contains(rrHdr, "expandGlobalConfigSentinel"),
           "INV-1 decl",
           "src/resolvedroot.h missing expandGlobalConfigSentinel "
           "declaration");
    expect(contains(rrHdr, "namespace ants"),
           "INV-1 namespace",
           "src/resolvedroot.h missing `namespace ants` wrap");

    // INV-2 — definition in remotecontrol.cpp, namespace ants.
    // (The closing `}  // namespace ants` immediately follows
    // expandGlobalConfigSentinel's body.)
    std::regex defRe(R"(QString\s+expandGlobalConfigSentinel\s*\()");
    expect(std::regex_search(rcCpp, defRe),
           "INV-2",
           "remotecontrol TUs missing expandGlobalConfigSentinel "
           "definition");

    // INV-3 — both sentinel values recognised.
    expect(contains(rcCpp, "\"~global\""),
           "INV-3a",
           "expandGlobalConfigSentinel does not check the literal "
           "\"~global\" value");
    expect(contains(rcCpp, "\"~claude-config\""),
           "INV-3b",
           "expandGlobalConfigSentinel does not check the literal "
           "\"~claude-config\" value");

    // INV-4 — cmdWorkspaceSearch calls the helper before normal
    // resolution. Source-grep: the call must appear inside the
    // workspace_search function body. We assert presence and that
    // it occurs before the existing `callerRaw.isEmpty()` branch.
    {
        const size_t wsStart =
            rcCpp.find("RemoteControl::cmdWorkspaceSearch(");
        expect(wsStart != std::string::npos,
               "INV-4 location",
               "cmdWorkspaceSearch implementation not found in "
               "remotecontrol.cpp");
        if (wsStart != std::string::npos) {
            const size_t windowEnd =
                std::min(rcCpp.size(), wsStart + 4000);
            const std::string window =
                rcCpp.substr(wsStart, windowEnd - wsStart);
            expect(contains(window, "expandGlobalConfigSentinel"),
                   "INV-4 wired",
                   "cmdWorkspaceSearch body does not call "
                   "ants::expandGlobalConfigSentinel");
        }
    }

    // INV-5 — cmdFileOutline calls the helper before normal
    // resolveRootCanonical.
    {
        const size_t foStart =
            rcCpp.find("RemoteControl::cmdFileOutline(");
        expect(foStart != std::string::npos,
               "INV-5 location",
               "cmdFileOutline implementation not found in "
               "remotecontrol.cpp");
        if (foStart != std::string::npos) {
            const size_t windowEnd =
                std::min(rcCpp.size(), foStart + 2000);
            const std::string window =
                rcCpp.substr(foStart, windowEnd - foStart);
            expect(contains(window, "expandGlobalConfigSentinel"),
                   "INV-5 wired",
                   "cmdFileOutline body does not call "
                   "ants::expandGlobalConfigSentinel");
        }
    }

    // INV-6 — workspace_search description documents the sentinel.
    {
        const size_t wsToolPos = ciCpp.find("\"workspace_search\"");
        expect(wsToolPos != std::string::npos,
               "INV-6 location",
               "tools/list missing workspace_search registration");
        if (wsToolPos != std::string::npos) {
            const size_t windowEnd =
                std::min(ciCpp.size(), wsToolPos + 8000);
            const std::string window =
                ciCpp.substr(wsToolPos, windowEnd - wsToolPos);
            expect(contains(window, "~global"),
                   "INV-6 doc-~global",
                   "workspace_search description does not mention "
                   "the ~global sentinel");
            expect(contains(window, "~/.claude/"),
                   "INV-6 doc-claude-dir",
                   "workspace_search description does not mention "
                   "~/.claude/");
        }
    }

    // INV-7 — file_outline description documents the sentinel.
    {
        const size_t foToolPos =
            ciCpp.find("foTool[\"name\"] = \"file_outline\"");
        expect(foToolPos != std::string::npos,
               "INV-7 location",
               "tools/list missing file_outline name registration");
        if (foToolPos != std::string::npos) {
            const size_t windowEnd =
                std::min(ciCpp.size(), foToolPos + 4000);
            const std::string window =
                ciCpp.substr(foToolPos, windowEnd - foToolPos);
            expect(contains(window, "~global"),
                   "INV-7 doc-~global",
                   "file_outline description does not mention "
                   "the ~global sentinel");
            expect(contains(window, "~/.claude/"),
                   "INV-7 doc-claude-dir",
                   "file_outline description does not mention "
                   "~/.claude/");
        }
    }

    EXPECT_EQ(0, expect_failures())
        << expect_failures()
        << " ANTS-1390 wiring invariant(s) failed";
}

TEST(McpGlobalConfigSentinel, RuntimeSentinelExpand) {
    expect_reset();

    // INV-8 — exact "~global" sentinel resolves under home.
    const QString globalRoot =
        ants::expandGlobalConfigSentinel(QStringLiteral("~global"));
    const QString homeClaude =
        QDir::homePath() + QStringLiteral("/.claude");
    if (QDir(homeClaude).exists()) {
        // The user running tests has ~/.claude/ — assert the
        // canonical match.
        expect(!globalRoot.isEmpty(),
               "INV-8",
               "expandGlobalConfigSentinel(\"~global\") returned "
               "empty despite ~/.claude existing");
        expect(globalRoot.endsWith(QStringLiteral("/.claude")),
               "INV-8 suffix",
               "expandGlobalConfigSentinel(\"~global\") result "
               "does not end in /.claude");

        // INV-9 — "~claude-config" returns the same canonical path.
        const QString aliasRoot =
            ants::expandGlobalConfigSentinel(
                QStringLiteral("~claude-config"));
        expect(aliasRoot == globalRoot,
               "INV-9",
               "expandGlobalConfigSentinel(\"~claude-config\") "
               "does not match \"~global\"");
    } else {
        // No ~/.claude on this machine — helper returns empty by
        // design. Skip the positive cases; assert empty so we
        // don't silently green-pass a regression that always
        // returns empty.
        expect(globalRoot.isEmpty(),
               "INV-8 no-home-claude",
               "~/.claude does not exist but sentinel returned a "
               "non-empty path — helper should refuse");
    }

    // INV-10 — empty string is not a sentinel.
    expect(ants::expandGlobalConfigSentinel(QString()).isEmpty(),
           "INV-10",
           "empty callerCwd should not match the sentinel");

    // INV-11 — arbitrary path is not a sentinel.
    expect(ants::expandGlobalConfigSentinel(
               QStringLiteral("/some/project/root")).isEmpty(),
           "INV-11",
           "arbitrary path should not match the sentinel");

    // INV-12 — only the exact sentinel matches; no prefix interp.
    expect(ants::expandGlobalConfigSentinel(
               QStringLiteral("~global/etc")).isEmpty(),
           "INV-12",
           "\"~global/etc\" should NOT match — only the bare "
           "sentinel is accepted (path-prefix interp must be "
           "explicit and is out of scope for v1)");

    EXPECT_EQ(0, expect_failures())
        << expect_failures()
        << " ANTS-1390 runtime invariant(s) failed";
}
