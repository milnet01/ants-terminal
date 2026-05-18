// Source-grep harness for ANTS-1248 — locks the wiring contract for
// the new `workspace_search` MCP tool (ripgrep wrapper). See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "../../_support/expect.h"

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
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
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

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}



}  // namespace

TEST(McpWorkspaceSearch, WiringContract) {
    expect_reset();

    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — cmdWorkspaceSearch declared public on RemoteControl
    // alongside the ANTS-1244 trio.
    expect(contains(rcHdr, "cmdWorkspaceSearch(const QJsonObject &req)"),
           "INV-1",
           "cmdWorkspaceSearch decl missing from src/remotecontrol.h "
           "(must sit next to cmdRoadmapQuery/cmdTabList/cmdGetText in "
           "the ANTS-1244 public block)");

    // INV-2 — body has 10 INV anchors, one per spec invariant.
    std::regex anchorRe(R"(//\s*ANTS-1248-INV-\d+)");
    auto begin = std::sregex_iterator(rcCpp.begin(), rcCpp.end(), anchorRe);
    auto end   = std::sregex_iterator();
    const long anchorCount = std::distance(begin, end);
    char detail2[160];
    std::snprintf(detail2, sizeof detail2,
                  "expected >=10 // ANTS-1248-INV-N anchors in "
                  "remotecontrol.cpp, found %ld",
                  anchorCount);
    expect(anchorCount >= 10, "INV-2", detail2);

    // INV-3 — shell-less argv. The body must invoke rg via
    // QProcess::start("rg", QStringList...) NOT via bash -c or the
    // single-string overload.
    expect(!contains(rcCpp, "bash -c"),
           "INV-3a",
           "remotecontrol.cpp contains 'bash -c' — shell-less argv "
           "violated");
    expect(!contains(rcCpp, "/bin/sh"),
           "INV-3b",
           "remotecontrol.cpp contains '/bin/sh' — shell-less argv "
           "violated");
    // Word-boundary regex so substrings inside identifiers (e.g.
    // `cmdSubsystem(` from ANTS-1251) are not false-flagged. We only
    // want to catch a bare call to ::system or std::system.
    {
        std::regex sysCall(R"(\bsystem\()");
        expect(!std::regex_search(rcCpp, sysCall),
               "INV-3c",
               "remotecontrol.cpp contains a bare 'system(' call — "
               "shell-less argv violated");
    }
    expect(!contains(rcCpp, "popen("),
           "INV-3d",
           "remotecontrol.cpp contains 'popen(' — shell-less argv "
           "violated");
    // Positive shape: the body uses QProcess::start with an argv list.
    // Either `start("rg", ...)` or `start(QStringLiteral("rg"), ...)`.
    std::regex rgStart(
        R"(\.start\s*\(\s*(QStringLiteral\s*\(\s*)?"rg")");
    expect(std::regex_search(rcCpp, rgStart),
           "INV-3e",
           "remotecontrol.cpp does not call QProcess::start(\"rg\", ...) "
           "with an argv list");

    // INV-4 — IPC dispatcher routes "workspace-search".
    expect(contains(rcCpp, "\"workspace-search\"") &&
           contains(rcCpp, "cmdWorkspaceSearch"),
           "INV-4",
           "remotecontrol.cpp dispatch missing \"workspace-search\" "
           "→ cmdWorkspaceSearch routing");

    // INV-5 — tools/list registers "workspace_search" with a real
    // input schema (not the shared emptySchema sentinel). Required
    // property: `pattern`.
    expect(contains(ciCpp, "\"workspace_search\""),
           "INV-5a",
           "tools/list missing \"workspace_search\" name registration");
    // The schema must declare pattern + the optional argument names.
    // We check for each property name as a JSON key literal.
    const char *requiredProps[] = {
        "\"pattern\"", "\"regex\"", "\"lane\"", "\"glob\"",
        "\"max_results\"", "\"context\"", "\"case\"",
    };
    for (const char *p : requiredProps) {
        char label[64];
        std::snprintf(label, sizeof label, "INV-5 prop %s", p);
        char d[160];
        std::snprintf(d, sizeof d,
                      "claudeintegration.cpp does not register %s in "
                      "workspace_search inputSchema.properties",
                      p);
        expect(contains(ciCpp, p), label, d);
    }
    // pattern must be in the required array (literal `"required"`
    // referencing `"pattern"`). Loose check: both literals appear in
    // proximity in the source. A strict regex over JSON construction
    // would be brittle. Window widened to 8000 after ANTS-1452 added
    // two new props (respect_gitignore + include_hidden) with their
    // own description blurbs, pushing the schema past the original
    // 4 KiB anchor. Widened to 10000 again after ANTS-1565 added
    // timeout_sec with its own description blurb.
    {
        // Anchor at the tools/list registration (literal "workspace_search"
        // with quotes), not the first incidental occurrence in a setter
        // comment.
        const size_t reqPos = ciCpp.find("\"workspace_search\"");
        bool ok = false;
        if (reqPos != std::string::npos) {
            const size_t windowEnd = std::min(ciCpp.size(),
                                              reqPos + 10000);
            const std::string window = ciCpp.substr(reqPos,
                                                    windowEnd - reqPos);
            ok = contains(window, "\"required\"") &&
                 contains(window, "\"pattern\"");
        }
        expect(ok, "INV-5 required",
               "workspace_search inputSchema does not declare "
               "[\"pattern\"] as required");
    }

    // INV-6 — tools/list schema declares the workspace_search tool.
    // ANTS-1253 collapsed the per-tool dispatch into a single registry
    // lookup, so we re-anchor the wiring check to the schema entry.
    std::regex schemaRe(R"("name"\]\s*=\s*"workspace_search")");
    expect(std::regex_search(ciCpp, schemaRe),
           "INV-6",
           "claudeintegration.cpp missing tools/list schema entry "
           "naming \"workspace_search\"");

    // INV-7 — header declares the single registry surface (ANTS-1253).
    // Per-tool setters / members no longer exist.
    expect(contains(ciHdr, "registerToolProvider(const QString &name"),
           "INV-7a",
           "claudeintegration.h missing registerToolProvider "
           "declaration (ANTS-1253)");
    expect(contains(ciHdr, "m_toolProviders"),
           "INV-7b",
           "claudeintegration.h missing m_toolProviders registry "
           "member (ANTS-1253)");
    std::regex toolHandlerRe(
        R"(using\s+ToolHandler\s*=\s*std::function\s*<\s*QString\s*\(\s*const\s+QJsonObject\s*&)");
    expect(std::regex_search(ciHdr, toolHandlerRe),
           "INV-7c",
           "claudeintegration.h does not declare ToolHandler as "
           "std::function<QString(const QJsonObject&)> — required for "
           "the workspace_search provider signature");

    // INV-8 — mainwindow.cpp registers workspace_search via the
    // ANTS-1253 registry and the lambda delegates to cmdWorkspaceSearch.
    expect(contains(mwCpp, "registerToolProvider(\"workspace_search\""),
           "INV-8a",
           "mainwindow.cpp does not register workspace_search "
           "in setupClaudeMcpProviders (ANTS-1253)");
    expect(contains(mwCpp, "cmdWorkspaceSearch"),
           "INV-8b",
           "mainwindow.cpp does not delegate the provider lambda to "
           "m_remoteControl->cmdWorkspaceSearch");

    // INV-9 — rg argv flags appear literally in the body. The exact
    // string forms are matched so a future refactor cannot drop them.
    const char *rgFlags[] = {
        "--json",
        "--no-heading",
        "--line-number",
        "--max-columns",
        "--threads",
    };
    for (const char *f : rgFlags) {
        char label[64];
        std::snprintf(label, sizeof label, "INV-9 flag %s", f);
        char d[160];
        std::snprintf(d, sizeof d,
                      "remotecontrol.cpp cmdWorkspaceSearch does not "
                      "pass `%s` to ripgrep — required by spec §3",
                      f);
        expect(contains(rcCpp, f), label, d);
    }

    // INV-10 — 2-tier kill timer: 2000 ms SIGTERM then 200 ms grace
    // before SIGKILL. We check that the body contains both
    // QTimer::singleShot(2000 (or a named constant set to that),
    // calls terminate(), and references the 200 ms grace.
    expect(contains(rcCpp, "QTimer::singleShot(2000") ||
           contains(rcCpp, "QTimer::singleShot( 2000") ||
           contains(rcCpp, "kWorkspaceSearchHardKillMs"),
           "INV-10a",
           "remotecontrol.cpp missing 2 s hard-kill timer for rg "
           "(QTimer::singleShot(2000, ...) or kWorkspaceSearchHardKillMs)");
    expect(contains(rcCpp, "->terminate(") ||
           contains(rcCpp, ".terminate("),
           "INV-10b",
           "remotecontrol.cpp does not call QProcess::terminate() on "
           "the rg child — INV-5 violated");
    expect(contains(rcCpp, "->kill(") ||
           contains(rcCpp, ".kill(") ||
           contains(rcCpp, "kWorkspaceSearchKillGraceMs"),
           "INV-10c",
           "remotecontrol.cpp does not call QProcess::kill() (the 200 ms "
           "grace step after terminate) on the rg child — INV-5 "
           "violated");

    // -- ANTS-1452 additions: gitignore-bypass + hidden-file opt-ins -----
    // Six new invariants riding the same wiring contract; the original
    // 10 above remain unchanged.

    // INV-1452-1a — argv contains "--no-ignore-vcs" literal.
    expect(contains(rcCpp, "\"--no-ignore-vcs\""),
           "INV-1452-1a",
           "remotecontrol.cpp does not pass --no-ignore-vcs — needed for "
           "respect_gitignore=false (ANTS-1452 INV-1)");
    // INV-1452-1b — argv contains "--no-ignore" literal (the umbrella).
    // Use a regex so the substring isn't matched against --no-ignore-vcs.
    {
        std::regex noIgnoreRe(R"("--no-ignore"\s*[,)])");
        expect(std::regex_search(rcCpp, noIgnoreRe),
               "INV-1452-1b",
               "remotecontrol.cpp does not pass the bare --no-ignore "
               "umbrella flag — needed for respect_gitignore=false "
               "(ANTS-1452 INV-1)");
    }
    // INV-1452-1c — the bypass is gated on the respect_gitignore arg.
    expect(contains(rcCpp, "respect_gitignore") ||
           contains(rcCpp, "respectGitignore"),
           "INV-1452-1c",
           "remotecontrol.cpp does not parse the respect_gitignore arg "
           "(ANTS-1452 INV-1)");

    // INV-1452-2 — --hidden gated on include_hidden.
    expect(contains(rcCpp, "\"--hidden\""),
           "INV-1452-2a",
           "remotecontrol.cpp does not pass --hidden — needed for "
           "include_hidden=true (ANTS-1452 INV-2)");
    expect(contains(rcCpp, "include_hidden") ||
           contains(rcCpp, "includeHidden"),
           "INV-1452-2b",
           "remotecontrol.cpp does not parse the include_hidden arg "
           "(ANTS-1452 INV-2)");

    // INV-1452-4 — ok:true envelope echoes both filter values.
    expect(contains(rcCpp, "out[\"respect_gitignore\"]"),
           "INV-1452-4a",
           "remotecontrol.cpp does not set out[\"respect_gitignore\"] "
           "on the ok:true envelope (ANTS-1452 INV-4)");
    expect(contains(rcCpp, "out[\"include_hidden\"]"),
           "INV-1452-4b",
           "remotecontrol.cpp does not set out[\"include_hidden\"] on "
           "the ok:true envelope (ANTS-1452 INV-4)");

    // INV-1452-5 — schema declares both new property names.
    expect(contains(ciCpp, "\"respect_gitignore\""),
           "INV-1452-5a",
           "claudeintegration.cpp does not register "
           "\"respect_gitignore\" in workspace_search inputSchema "
           "(ANTS-1452 INV-5)");
    expect(contains(ciCpp, "\"include_hidden\""),
           "INV-1452-5b",
           "claudeintegration.cpp does not register \"include_hidden\" "
           "in workspace_search inputSchema (ANTS-1452 INV-5)");

    // INV-1452-6 — parse uses the default-preserving toBool overload.
    // Source-scrape for `.toBool(true)` and `.toBool(false)` near each
    // flag name to lock in the defaults.
    {
        std::regex rgIgnoreDefault(
            R"(respect_gitignore[^;]{0,80}\.toBool\(\s*true\s*\))");
        expect(std::regex_search(rcCpp, rgIgnoreDefault),
               "INV-1452-6a",
               "remotecontrol.cpp does not call .toBool(true) on the "
               "respect_gitignore parse — default must preserve "
               "pre-1452 behaviour (ANTS-1452 INV-6)");
        std::regex rgHiddenDefault(
            R"(include_hidden[^;]{0,80}\.toBool\(\s*false\s*\))");
        expect(std::regex_search(rcCpp, rgHiddenDefault),
               "INV-1452-6b",
               "remotecontrol.cpp does not call .toBool(false) on the "
               "include_hidden parse — default must preserve pre-1452 "
               "behaviour (ANTS-1452 INV-6)");
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1248 / ANTS-1452 invariant(s) failed";
}
