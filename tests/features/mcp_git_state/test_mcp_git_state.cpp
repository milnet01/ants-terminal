// Source-grep harness for ANTS-1250 — locks the wiring contract for
// the new `git_state` consolidated MCP tool (status/log/diff). See
// spec.md.
//
// Exit 0 = all 13 invariants hold.

#include "../../_support/expect.h"

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
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_GITWRAP_CPP_PATH
#error "SRC_GITWRAP_CPP_PATH compile definition required"
#endif
#ifndef SRC_GITWRAP_H_PATH
#error "SRC_GITWRAP_H_PATH compile definition required"
#endif
#ifndef CMAKELISTS_PATH
#error "CMAKELISTS_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}



}  // namespace

TEST(McpGitState, WiringContract) {
    expect_reset();

    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string gwCpp = ants_test::slurpFile(SRC_GITWRAP_CPP_PATH);
    const std::string gwHdr = ants_test::slurpFile(SRC_GITWRAP_H_PATH);
    const std::string cmake = ants_test::slurpFile(CMAKELISTS_PATH);

    // INV-1 — cmdGitState declared public on RemoteControl alongside
    // the ANTS-1244/1248/1249 trio.
    expect(contains(rcHdr, "cmdGitState(const QJsonObject &req)"),
           "INV-1",
           "cmdGitState decl missing from src/remotecontrol.h "
           "(must sit next to cmdWorkspaceSearch / cmdFileOutline in "
           "the public block)");

    // INV-2 — body has 13 INV anchors (one per spec invariant), spread
    // across remotecontrol.cpp + gitwrap.cpp.
    std::regex anchorRe(R"(//\s*ANTS-1250-INV-\d+)");
    auto countAnchors = [&](const std::string &s) -> long {
        auto begin = std::sregex_iterator(s.begin(), s.end(), anchorRe);
        auto end   = std::sregex_iterator();
        return std::distance(begin, end);
    };
    const long anchorCount = countAnchors(rcCpp) + countAnchors(gwCpp) +
                             countAnchors(gwHdr);
    char detail2[200];
    std::snprintf(detail2, sizeof detail2,
                  "expected >=13 // ANTS-1250-INV-N anchors across "
                  "remotecontrol.cpp + gitwrap.{cpp,h}, found %ld",
                  anchorCount);
    expect(anchorCount >= 13, "INV-2", detail2);

    // INV-3 — shell-less argv. Anyone who forks git in this codebase
    // must do so via gitwrap.cpp's QProcess argv form. We assert that
    // gitwrap.cpp does not contain shell escapes.
    expect(!contains(gwCpp, "bash -c"),
           "INV-3a",
           "gitwrap.cpp contains 'bash -c' — shell-less argv violated");
    expect(!contains(gwCpp, "/bin/sh"),
           "INV-3b",
           "gitwrap.cpp contains '/bin/sh' — shell-less argv violated");
    expect(!contains(gwCpp, "system("),
           "INV-3c",
           "gitwrap.cpp contains 'system(' — shell-less argv violated");
    expect(!contains(gwCpp, "popen("),
           "INV-3d",
           "gitwrap.cpp contains 'popen(' — shell-less argv violated");

    // INV-4 — gitwrap.cpp invokes QProcess::start("git", argv).
    std::regex gitStart(
        R"(\.start\s*\(\s*(QStringLiteral\s*\(\s*)?"git")");
    expect(std::regex_search(gwCpp, gitStart),
           "INV-4",
           "gitwrap.cpp does not call QProcess::start(\"git\", ...) "
           "with an argv list");

    // INV-5 — IPC dispatcher routes "git-state".
    expect(contains(rcCpp, "\"git-state\"") &&
           contains(rcCpp, "cmdGitState"),
           "INV-5",
           "remotecontrol.cpp dispatch missing \"git-state\" → "
           "cmdGitState routing");

    // INV-6 — tools/list registers a single "git_state" entry with
    // op enum {status, log, diff}. ANTS-3365 — op is no longer in
    // required[] (it defaults to "status"); the schema advertises that.
    expect(contains(ciCpp, "\"git_state\""),
           "INV-6a",
           "tools/list missing \"git_state\" name registration");
    {
        // Window widened to 6400 (ANTS-3365 lengthened the description +
        // opProp; ANTS-3377 added hunks/staged/include_lines/context props
        // ahead of the `props["op"]` assignment); a fixed scrape window must
        // outrun added schema text.
        const size_t pos = ciCpp.find("\"git_state\"");
        bool ok = false;
        if (pos != std::string::npos) {
            const size_t windowEnd = std::min(ciCpp.size(), pos + 6400);
            const std::string window = ciCpp.substr(pos, windowEnd - pos);
            ok = contains(window, "\"status\"") &&
                 contains(window, "\"log\"") &&
                 contains(window, "\"diff\"") &&
                 contains(window, "\"op\"") &&
                 // op default surfaced in-schema (ANTS-3365).
                 contains(window, "\"default\"");
        }
        expect(ok, "INV-6b",
               "git_state inputSchema does not declare op enum "
               "{status,log,diff} + op default (ANTS-3365)");
    }

    // INV-7 — tools/list schema declares the git_state tool.
    // ANTS-1253 collapsed the per-tool dispatch into a registry lookup.
    std::regex schemaRe(R"("name"\]\s*=\s*"git_state")");
    expect(std::regex_search(ciCpp, schemaRe),
           "INV-7",
           "claudeintegration.cpp missing tools/list schema entry for git_state");

    // INV-8 — header has the single registry surface (ANTS-1253).
    expect(contains(ciHdr, "registerToolProvider(const QString &name"),
           "INV-8a",
           "claudeintegration.h missing registerToolProvider declaration (ANTS-1253)");
    expect(contains(ciHdr, "m_toolProviders"),
           "INV-8b",
           "claudeintegration.h missing m_toolProviders registry member (ANTS-1253)");

    // INV-9 — mainwindow.cpp registers git_state via the registry.
    expect(contains(mwCpp, "registerToolProvider(\"git_state\""),
           "INV-9a",
           "mainwindow.cpp does not register git_state in "
           "setupClaudeMcpProviders (ANTS-1253)");
    expect(contains(mwCpp, "cmdGitState"),
           "INV-9b",
           "mainwindow.cpp does not delegate the provider lambda to "
           "m_remoteControl->cmdGitState");

    // INV-10 — cmdGitState dispatches on op ∈ {status, log, diff}.
    expect(contains(rcCpp, "\"status\"") &&
           contains(rcCpp, "\"log\"") &&
           contains(rcCpp, "\"diff\""),
           "INV-10a",
           "remotecontrol.cpp does not dispatch on op string literals "
           "{status, log, diff}");
    // bad_op error code surfaced for unknown op.
    expect(contains(rcCpp, "\"bad_op\""),
           "INV-10b",
           "remotecontrol.cpp does not surface bad_op error for "
           "unknown op (spec § 4 INV-1)");
    // ANTS-3365 — an omitted op defaults to "status" (a bare
    // git_state{caller_cwd} returns the one-call status read). The
    // dispatch is `if (op.isEmpty()) op = ... "status"` before the
    // {status,log,diff} branches; a non-empty unknown op still bad_op.
    expect(contains(rcCpp, "op.isEmpty()") &&
           contains(rcCpp, "ANTS-3365"),
           "INV-10c",
           "remotecontrol.cpp cmdGitState does not default an omitted "
           "op to \"status\" (ANTS-3365)");
    // The schema must drop op from `required` and advertise the default.
    expect(contains(ciCpp, "ANTS-3365"),
           "INV-10d",
           "claudeintegration.cpp git_state schema does not mark op "
           "optional / default \"status\" (ANTS-3365)");

    // INV-11 — stricter range regex closes leading-`-` flag-injection.
    // The regex literal in source must use `[a-zA-Z0-9._/^~]` (no `-`)
    // as the first character class of a rev-component. Spec § 4 INV-4.
    expect(contains(rcCpp, "[a-zA-Z0-9._/^~]"),
           "INV-11a",
           "remotecontrol.cpp range regex does not exclude `-` from "
           "the first character of a rev-component (cold-eyes "
           "S1250-1 leading-flag injection)");
    // bad_range error code reachable.
    expect(contains(rcCpp, "\"bad_range\""),
           "INV-11b",
           "remotecontrol.cpp does not surface bad_range error code");

    // ANTS-2074 — op:diff with no `range` defaults to the working-tree
    // diff (bare `git diff`) instead of refusing with bad_range. The
    // worktreeDiff flag gates whether `range` is appended to argv.
    expect(contains(rcCpp, "worktreeDiff"),
           "ANTS-2074",
           "runDiffOp does not implement the working-tree default "
           "(worktreeDiff) for an omitted range");

    // INV-12 — gitwrap uses 5 s + 200 ms two-tier kill timer
    // (kHardKillMs / kKillGraceMs) and calls terminate() + kill().
    expect(contains(gwCpp, "kHardKillMs"),
           "INV-12a",
           "gitwrap.cpp missing 5 s hard-kill constant (kHardKillMs)");
    expect(contains(gwCpp, "kKillGraceMs"),
           "INV-12b",
           "gitwrap.cpp missing 200 ms grace constant (kKillGraceMs)");
    expect(contains(gwCpp, ".terminate("),
           "INV-12c",
           "gitwrap.cpp does not call QProcess::terminate()");
    expect(contains(gwCpp, ".kill("),
           "INV-12d",
           "gitwrap.cpp does not call QProcess::kill()");

    // INV-13 — CMake wires src/gitwrap.cpp into the build.
    expect(contains(cmake, "src/gitwrap.cpp"),
           "INV-13",
           "CMakeLists.txt does not list src/gitwrap.cpp in "
           "ants_core_lib SOURCES");

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1250 invariant(s) failed";
}
