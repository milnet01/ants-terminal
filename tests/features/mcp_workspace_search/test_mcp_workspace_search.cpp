// Source-grep harness for ANTS-1248 — locks the wiring contract for
// the new `workspace_search` MCP tool (ripgrep wrapper). See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "../../_support/expect.h"
#include "rgdiagnosis.h"
#include "remotecontrol.h"
#include "mcpspill.h"   // ANTS-4892

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "../../_support/srcregex.h"
#include "../../_support/xdg_guard.h"

#ifndef ANTS_CLAUDEINTEGRATION_SOURCES
#error "ANTS_CLAUDEINTEGRATION_SOURCES compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_H_PATH
#error "SRC_CLAUDE_INTEGRATION_H_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef ANTS_MAINWINDOW_SOURCES
#error "ANTS_MAINWINDOW_SOURCES compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}



}  // namespace

// ANTS-6362 — these scrapes use PCRE2, not std::regex: see srcregex.h.
using ants_test::srcCount;
using ants_test::srcSearch;

TEST(McpWorkspaceSearch, WiringContract) {
    expect_reset();

    const std::string ciCpp = ants_test::slurpClaudeIntegration();
    const std::string ciHdr = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string mwCpp = ants_test::slurpMainWindow();

    // INV-1 — cmdWorkspaceSearch declared public on RemoteControl
    // alongside the ANTS-1244 trio.
    expect(contains(rcHdr, "cmdWorkspaceSearch(const QJsonObject &req)"),
           "INV-1",
           "cmdWorkspaceSearch decl missing from src/remotecontrol.h "
           "(must sit next to cmdRoadmapQuery/cmdTabList/cmdGetText in "
           "the ANTS-1244 public block)");

    // INV-2 — body has 10 INV anchors, one per spec invariant.
    const long anchorCount = srcCount(rcCpp, R"(//\s*ANTS-1248-INV-\d+)");
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
        expect(!srcSearch(rcCpp, R"(\bsystem\()"),
               "INV-3c",
               "remotecontrol.cpp contains a bare 'system(' call — "
               "shell-less argv violated");
    }
    expect(!contains(rcCpp, "popen("),
           "INV-3d",
           "remotecontrol.cpp contains 'popen(' — shell-less argv "
           "violated");
    // Positive shape: the body uses QProcess::start with an argv list.
    // Either `start("rg", ...)` or `start(QStringLiteral("rg"), ...)`, or
    // the ANTS-5527 host-aware `HostExec::start(proc, QStringLiteral("rg"), ...)`.
    expect(srcSearch(rcCpp, R"((\.start\s*\(|HostExec::start\s*\(\s*\w+\s*,)\s*(QStringLiteral\s*\(\s*)?"rg")"),
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
    // timeout_sec with its own description blurb. Widened to 16000
    // after ANTS-2041 added the `query` alias prop (its own blurb
    // pushed `required.append("pattern")` to ~14.6 KiB past the anchor).
    // Widened to 18000 after ANTS-2220 added the `enclosing_symbol` prop +
    // its description blurb and a detail-field paragraph.
    // Widened to 20000 after ANTS-3537 added the `count_only` prop + its
    // description blurb and a detail-field mention.
    // Widened to 22000 after ANTS-3549 added the `files_only` prop + its
    // description blurb and a detail-field mention.
    // Widened to 24000 after ANTS-3547 added the `offset` prop (wsOffsetProp)
    // + its description blurb and a detail-field mention (required.append at
    // offset ~22774).
    {
        // Anchor at the tools/list registration (literal "workspace_search"
        // with quotes), not the first incidental occurrence in a setter
        // comment.
        // ANTS-3720 — the descriptor block bounds itself. This was a
        // fixed-byte window whose last widening (24000 → 32000, ANTS-3704)
        // reddened the row with a message about `pattern` that had nothing to
        // do with the change that broke it — the failure mode the idiom keeps
        // producing.
        const std::string window =
            ants_test::mcpToolDescriptor(ciCpp, "workspace_search");
        const bool ok = !window.empty() &&
                        contains(window, "\"required\"") &&
                        contains(window, "\"pattern\"");
        expect(ok, "INV-5 required",
               "workspace_search inputSchema does not declare "
               "[\"pattern\"] as required");
    }

    // INV-6 — tools/list schema declares the workspace_search tool.
    // ANTS-1253 collapsed the per-tool dispatch into a single registry
    // lookup, so we re-anchor the wiring check to the schema entry.
    expect(srcSearch(ciCpp, R"("name"\]\s*=\s*"workspace_search")"),
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
    // ANTS-4932 § 2.3 — defined in mcptoolsink.h, aliased by ClaudeIntegration.
    expect(srcSearch(ants_test::slurpFile(SRC_MCP_TOOL_SINK_H_PATH),
                     R"(using\s+ToolHandler\s*=\s*std::function\s*<\s*)"
                     R"(QString\s*\(\s*const\s+QJsonObject\s*&)") &&
               contains(ciHdr, "using ToolHandler = mcp::ToolHandler;"),
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
        expect(srcSearch(rcCpp, R"("--no-ignore"\s*[,)])"),
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
        expect(srcSearch(rcCpp, R"(respect_gitignore[^;]{0,80}\.toBool\(\s*true\s*\))"),
               "INV-1452-6a",
               "remotecontrol.cpp does not call .toBool(true) on the "
               "respect_gitignore parse — default must preserve "
               "pre-1452 behaviour (ANTS-1452 INV-6)");
        expect(srcSearch(rcCpp, R"(include_hidden[^;]{0,80}\.toBool\(\s*false\s*\))"),
               "INV-1452-6b",
               "remotecontrol.cpp does not call .toBool(false) on the "
               "include_hidden parse — default must preserve pre-1452 "
               "behaviour (ANTS-1452 INV-6)");
    }

    // -- ANTS-1304 additions: surrounding-context lines on each match ---
    // Four invariants riding the same wiring contract.

    // INV-1304-1 — body parses type=="context" events with a
    // context > 0 guard.
    {
        expect(srcSearch(rcCpp, R"(QLatin1String\("context"\))"),
               "INV-1304-1a",
               "remotecontrol.cpp does not branch on the rg "
               "type==\"context\" event — ANTS-1304 INV-1 violated");
        // The guard must be paired with `context > 0` in the
        // immediate vicinity (within ~400 chars).
        const auto p = rcCpp.find("QLatin1String(\"context\")");
        bool guardedOk = false;
        if (p != std::string::npos) {
            const auto windowEnd = std::min(rcCpp.size(), p + 400);
            const std::string window = rcCpp.substr(p, windowEnd - p);
            guardedOk = contains(window, "context > 0") ||
                        contains(window, "context>0");
        }
        expect(guardedOk,
               "INV-1304-1b",
               "remotecontrol.cpp context branch is not paired with a "
               "`context > 0` guard — ANTS-1304 INV-1 violated");
    }

    // INV-1304-2 — match emit assigns both context_before and
    // context_after on each match when context > 0.
    expect(contains(rcCpp, "\"context_before\""),
           "INV-1304-2a",
           "remotecontrol.cpp does not assign m[\"context_before\"] "
           "— ANTS-1304 INV-2 violated");
    expect(contains(rcCpp, "\"context_after\""),
           "INV-1304-2b",
           "remotecontrol.cpp does not assign m[\"context_after\"] "
           "— ANTS-1304 INV-2 violated");

    // INV-1304-6 — tools/list description mentions both new fields
    // AND the [0,10] / server-clamped language. The relevant
    // description blurb lives in the workspace_search registration
    // block — anchor by name, then scan ~3 KiB forward.
    {
        // ANTS-4919 — the descriptors live in their own files, after the
        // handler code, so search only the descriptor pieces.
        const std::string desc = ants_test::toolSchemaPiecesText(ciCpp);
        const auto p = desc.find("\"workspace_search\"");
        bool ok = false;
        if (p != std::string::npos) {
            const auto windowEnd = std::min(desc.size(), p + 4000);
            const std::string window = desc.substr(p,
                                                    windowEnd - p);
            ok = contains(window, "context_before") &&
                 contains(window, "context_after") &&
                 (contains(window, "[0,10]") ||
                  contains(window, "[0, 10]") ||
                  contains(window, "server-clamped"));
        }
        expect(ok,
               "INV-1304-6",
               "workspace_search tools/list description does not "
               "mention context_before/context_after AND the [0,10] "
               "server clamp — ANTS-1304 INV-6 violated");
    }

    // INV-1304-7 — argv "--context" is gated on context > 0 (the
    // pre-1304 default-0 path passes no flag).
    {
        expect(srcSearch(rcCpp, R"(context\s*>\s*0[\s\S]{0,200}"--context")"),
               "INV-1304-7",
               "remotecontrol.cpp does not gate the rg --context "
               "flag on `context > 0` — ANTS-1304 INV-7 violated");
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1248 / ANTS-1452 / ANTS-1304 invariant(s) failed";
}

// ANTS-3704 — exclude_glob wiring. Three rows, and the ORDER row is the one
// that matters: ripgrep resolves competing globs last-one-wins, so an
// exclusion rendered BEFORE the positive `glob` would be overridden by it and
// the arg would silently do nothing. A test that only checked the flag exists
// would pass against exactly that bug.
TEST(McpWorkspaceSearch, Ants3704ExcludeGlobWiring) {
    expect_reset();
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string ciCpp = ants_test::slurpClaudeIntegration();

    expect(contains(rcCpp, "exclude_glob"),
           "INV-3704-1",
           "remotecontrol.cpp does not read the \"exclude_glob\" argument");

    // Rendered as a '!'-prefixed --glob, which is ripgrep's exclusion form.
    expect(srcSearch(rcCpp, R"(excludeGlobs[\s\S]{0,200}"--glob"[\s\S]{0,80}'!')"),
           "INV-3704-2",
           "exclude_glob entries are not rendered as ripgrep --glob '!<pattern>'");

    // Order: the positive glob's argv append must precede the exclusion loop.
    const size_t posGlob = rcCpp.find("if (!glob.isEmpty()) argv");
    const size_t exGlob  = rcCpp.find("for (const QString &ex : std::as_const(excludeGlobs))");
    expect(posGlob != std::string::npos && exGlob != std::string::npos
               && posGlob < exGlob,
           "INV-3704-3",
           "the exclude_glob argv loop does not follow the positive --glob "
           "append — last-one-wins means the exclusion would be overridden");

    // Declared in the schema, or the dispatcher reports it in ignored_args and
    // the caller is silently given an unfiltered result set (the ANTS-3698
    // failure shape, one verb over).
    expect(contains(ciCpp, "props[\"exclude_glob\"]"),
           "INV-3704-4",
           "workspace_search inputSchema does not declare exclude_glob");

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-3704 invariant(s) failed";
}

// ANTS-4650 — the refusal named the one cause a session can rule out, and
// offered no other. `rg_failed` has now been produced by two OPPOSITE causes:
// ripgrep genuinely absent (CI red on main, 2026-08-14, recorded in
// tests/features/ci_workflow_deps/spec.md) and ripgrep present and working
// (2026-08-25). A message that cannot separate them sends the reader to the
// wrong repair in one of the two cases, every time.
TEST(McpWorkspaceSearch, Ants4650StartFailureNamesTheRightCause) {
    using RgDiagnosis::StartFailure;

    StartFailure absent;                       // the 2026-08-14 incident
    absent.pathEnv        = QStringLiteral("/usr/bin:/bin");
    absent.startTimeoutMs = 500;
    const QString a = RgDiagnosis::explain(absent);
    EXPECT_TRUE(a.contains(QStringLiteral("not found on PATH")));
    EXPECT_TRUE(a.contains(QStringLiteral("/usr/bin:/bin")))
        << "must name the PATH it actually searched";

    StartFailure loaded;                       // the 2026-08-25 report
    loaded.exePath        = QStringLiteral("/usr/bin/rg");
    loaded.stillStarting  = true;
    loaded.startTimeoutMs = 500;
    const QString b = RgDiagnosis::explain(loaded);
    EXPECT_FALSE(b.contains(QStringLiteral("not found on PATH")))
        << "ripgrep was installed and working — do not send the reader there";
    EXPECT_TRUE(b.contains(QStringLiteral("/usr/bin/rg")));
    EXPECT_TRUE(b.contains(QStringLiteral("500")))
        << "a start-wait timeout must name the budget it exceeded";

    StartFailure failed;                       // found, exec refused
    failed.exePath     = QStringLiteral("/usr/bin/rg");
    failed.errorString = QStringLiteral("Resource temporarily unavailable");
    const QString c = RgDiagnosis::explain(failed);
    EXPECT_FALSE(c.contains(QStringLiteral("not found on PATH")));
    EXPECT_TRUE(c.contains(QStringLiteral("Resource temporarily unavailable")))
        << "the OS reason is the whole diagnostic value here";

    StartFailure gone;                         // cwd deleted before the spawn
    gone.exePath          = QStringLiteral("/usr/bin/rg");
    gone.workingDir       = QStringLiteral("/tmp/deleted-root");
    gone.workingDirExists = false;
    const QString d = RgDiagnosis::explain(gone);
    EXPECT_TRUE(d.contains(QStringLiteral("/tmp/deleted-root")));

    // Four causes, four repairs — none may read as another.
    const QStringList all{a, b, c, d};
    for (int i = 0; i < all.size(); ++i)
        for (int j = i + 1; j < all.size(); ++j)
            EXPECT_NE(all.at(i), all.at(j))
                << "cause " << i << " and " << j << " are indistinguishable";
}

// ANTS-4650 — every rg call site must route through the shared diagnosis. The
// hardcoded "(is ripgrep installed?)" is the string that could only ever name
// one cause, and there were three copies of it.
TEST(McpWorkspaceSearch, Ants4650NoCallSiteHardcodesTheOneCause) {
    const std::string rc = ants_test::slurpRemoteControl();
    // The emitted string, closing paren included — prose ABOUT the retired
    // message is not a reintroduction of it, and a guard that cannot tell the
    // difference reddens on its own explanation.
    EXPECT_EQ(rc.find("rg failed to start (is ripgrep installed?)"),
              std::string::npos)
        << "a call site still hardcodes the single-cause message";
}

// ANTS-5117 — a glob holding `/` under a lane matched nothing. rg runs from
// the project root with the lane as its path argument, and such a glob
// anchors at rg's working directory. Behavioural, through the real verb and
// ripgrep, against a fixture tree.
// ANTS-4855 — an unknown lane still refuses, and names the nearest directories.
TEST(McpWorkspaceSearch, Ants4855UnknownLaneNamesNearbyDirectories) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    for (const char *dir : {"src", "tests/alpha", "tests/features", "tests/fixtures"})
        ASSERT_TRUE(QDir(root).mkpath(QString::fromUtf8(dir)));
    QFile notADir(root + QStringLiteral("/tests/featurez"));
    ASSERT_TRUE(notADir.open(QIODevice::WriteOnly));
    notADir.close();

    const auto refuse = [&root](const QString &lane) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("pattern")] = QStringLiteral("x");
        req[QStringLiteral("lane")] = lane;
        RemoteControl rc(nullptr);
        const QJsonObject o = rc.cmdWorkspaceSearch(req).object();
        EXPECT_EQ(o.value(QStringLiteral("code")).toString(), QStringLiteral("bad_path"))
            << QJsonDocument(o).toJson().constData();
        return o.value(QStringLiteral("candidates")).toArray();
    };

    // A singular slip: the closest name comes first, a file is not offered.
    const QJsonArray singular = refuse(QStringLiteral("tests/feature"));
    EXPECT_EQ(singular.at(0).toString(), QStringLiteral("tests/features"));
    EXPECT_FALSE(singular.contains(QStringLiteral("tests/featurez")));
    EXPECT_TRUE(singular.contains(QStringLiteral("tests/alpha")));
    // A missing parent is answered from the deepest folder that exists.
    EXPECT_EQ(refuse(QStringLiteral("tst/features")).at(0).toString(), QStringLiteral("tests"));
}

TEST(McpWorkspaceSearch, Ants5117SlashGlobResolvesUnderLane) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    for (const char *rel : {"tests/features/alpha/a.cpp",
                            "tests/features/beta/b.cpp",
                            "tests/features/gamma/c.cpp"}) {
        const QString p = root + QLatin1Char('/') + QString::fromUtf8(rel);
        QDir().mkpath(QFileInfo(p).path());
        QFile f(p);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("needle_5117\n");
    }

    const auto count = [&root](const QJsonObject &extra) {
        QJsonObject req = extra;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("pattern")] = QStringLiteral("needle_5117");
        req[QStringLiteral("count_only")] = true;
        RemoteControl rc(nullptr);
        const QJsonObject o = rc.cmdWorkspaceSearch(req).object();
        EXPECT_TRUE(o.value(QStringLiteral("ok")).toBool())
            << QJsonDocument(o).toJson().constData();
        return o.value(QStringLiteral("count")).toInt(-1);
    };
    const QString lane = QStringLiteral("tests/features");

    EXPECT_EQ(count({{QStringLiteral("lane"), lane},
                     {QStringLiteral("glob"), QStringLiteral("{alpha,beta}/*.cpp")}}),
              2)
        << "a slash glob under a lane resolves against the lane - the reported defect";
    EXPECT_EQ(count({{QStringLiteral("glob"),
                      QStringLiteral("tests/features/{alpha,beta}/*.cpp")}}),
              2)
        << "the root-relative spelling with no lane is unchanged";
    EXPECT_EQ(count({{QStringLiteral("lane"), lane},
                     {QStringLiteral("glob"),
                      QStringLiteral("tests/features/{alpha,beta}/*.cpp")}}),
              2)
        << "a glob that already names the lane is not prefixed twice";
    EXPECT_EQ(count({{QStringLiteral("lane"), lane},
                     {QStringLiteral("exclude_glob"), QStringLiteral("gamma/**")}}),
              2)
        << "a slash exclude_glob under a lane excludes what it names";
    EXPECT_EQ(count({{QStringLiteral("lane"), lane},
                     {QStringLiteral("glob"), QStringLiteral("*.cpp")}}),
              3)
        << "a glob with no slash still matches at any depth";
}

// ANTS-5711 — a search that times out keeps what it found, in every mode.
// A fake rg on PATH prints one file's begin + match events, then hangs past
// the 1 s budget, so the hard kill lands with a result already parsed.
// files_only, matches_only and count_only build no matches[] rows, and the
// hard-kill guard tested matches[] alone, so they answered rg_failed.
TEST(McpWorkspaceSearch, Ants5711HardKillKeepsPartialResultsInEveryMode) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const QString hit = root + QStringLiteral("/a.txt");
    {
        QFile f(hit);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("needle_5711\n");
    }
    QTemporaryDir shimDir;
    ASSERT_TRUE(shimDir.isValid());
    {
        QFile s(shimDir.path() + QStringLiteral("/rg"));
        ASSERT_TRUE(s.open(QIODevice::WriteOnly));
        const QString path = QStringLiteral("{\"text\":\"%1\"}").arg(hit);
        s.write(QStringLiteral(
                    "#!/bin/sh\n"
                    "printf '%s\\n' '{\"type\":\"begin\",\"data\":{\"path\":%1}}'\n"
                    "printf '%s\\n' '{\"type\":\"match\",\"data\":{\"path\":%1,"
                    "\"lines\":{\"text\":\"needle_5711\\\\n\"},\"line_number\":1,"
                    "\"absolute_offset\":0,\"submatches\":[{\"match\":{\"text\":"
                    "\"needle_5711\"},\"start\":0,\"end\":11}]}}'\n"
                    "exec sleep 30\n")
                    .arg(path)
                    .toUtf8());
        s.close();
        s.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                         | QFileDevice::ExeOwner);
    }
    // Restores PATH on every exit, the ASSERT below included, before
    // shimDir is deleted.
    ants_test::XdgGuard env;
    env.setEnv("PATH", shimDir.path().toUtf8() + ":" + qgetenv("PATH"));

    const auto run = [&root](const char *mode) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = root;
        req[QStringLiteral("pattern")] = QStringLiteral("needle_5711");
        req[QStringLiteral("timeout_sec")] = 1;
        if (mode) req[QString::fromLatin1(mode)] = true;
        RemoteControl rc(nullptr);
        return rc.cmdWorkspaceSearch(req).object();
    };
    for (const char *mode : {static_cast<const char *>(nullptr), "files_only",
                             "matches_only", "count_only"}) {
        const QJsonObject o = run(mode);
        const char *name = mode ? mode : "rows";
        EXPECT_TRUE(o.value(QStringLiteral("ok")).toBool())
            << name << ": a timed-out search with a result in hand failed: "
            << QJsonDocument(o).toJson(QJsonDocument::Compact).constData();
        EXPECT_TRUE(o.value(QStringLiteral("truncated")).toBool())
            << name << ": a cut-short answer must say it is cut short";
    }
    const QJsonObject files = run("files_only");
    ASSERT_EQ(files.value(QStringLiteral("files")).toArray().size(), 1);
    EXPECT_EQ(files.value(QStringLiteral("files")).toArray().at(0).toObject()
                  .value(QStringLiteral("file")).toString(),
              QStringLiteral("a.txt"));
    EXPECT_EQ(run("count_only").value(QStringLiteral("count")).toInt(), 1);
}

// ANTS-5226 — a run of rg that failed with nothing found refuses; it never
// answers ok:true with no matches. Each shim fails one way: an error exit, a
// crash, and a hang past the wall budget.
TEST(McpWorkspaceSearch, Ants5226FailedRunNeverAnswersEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    {
        QFile f(root + QStringLiteral("/a.txt"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("needle_5226(\n");
    }
    for (const char *body : {"echo 'rg: broken' >&2; exit 2",
                             "kill -9 $$",
                             "exec sleep 30"}) {
        QTemporaryDir shimDir;
        ASSERT_TRUE(shimDir.isValid());
        {
            QFile s(shimDir.path() + QStringLiteral("/rg"));
            ASSERT_TRUE(s.open(QIODevice::WriteOnly));
            s.write(QByteArray("#!/bin/sh\n") + body + "\n");
            s.close();
            s.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                             | QFileDevice::ExeOwner);
        }
        ants_test::XdgGuard env;
        env.setEnv("PATH", shimDir.path().toUtf8() + ":" + qgetenv("PATH"));
        for (const char *mode : {"rows", "files_only", "matches_only", "count_only",
                                 "as_reported"}) {
            QJsonObject req;
            req[QStringLiteral("caller_cwd")] = root;
            req[QStringLiteral("pattern")] = QStringLiteral("needle_5226(");
            req[QStringLiteral("timeout_sec")] = 1;
            if (qstrcmp(mode, "as_reported") == 0) {
                // The 2026-09-17 call: context and enclosing_symbol on.
                req[QStringLiteral("context")] = 10;
                req[QStringLiteral("enclosing_symbol")] = true;
            } else if (qstrcmp(mode, "rows") != 0) {
                req[QLatin1String(mode)] = true;
            }
            RemoteControl rc(nullptr);
            const QJsonObject o = rc.cmdWorkspaceSearch(req).object();
            ASSERT_TRUE(o.contains(QStringLiteral("ok"))) << body << " / " << mode;
            EXPECT_FALSE(o.value(QStringLiteral("ok")).toBool())
                << "shim `" << body << "`, mode " << mode
                << ": a failed rg run answered as a clean search: "
                << QJsonDocument(o).toJson(QJsonDocument::Compact).constData();
        }
    }
}

namespace {
// A project with one hit in a hidden directory and one in a plain directory.
QJsonObject search6188(const QString &root, QJsonObject req) {
    req[QStringLiteral("caller_cwd")] = root;
    RemoteControl rc(nullptr);
    return rc.cmdWorkspaceSearch(req).object();
}

QString hiddenFixture(QTemporaryDir &tmp) {
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    for (const char *rel : {"/.github/ci.yml", "/.git/config", "/src/a.txt", "/src/b.md"}) {
        QDir().mkpath(QFileInfo(root + QLatin1String(rel)).path());
        QFile f(root + QLatin1String(rel));
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(rel).endsWith("ci.yml") ? "hidden_6188\n" : "plain_6188 next\n");
    }
    return root;
}
} // namespace

TEST(McpWorkspaceSearch, Ants6188HiddenDirsAreNamed) {
    QTemporaryDir tmp;
    const QString root = hiddenFixture(tmp);
    for (const char *mode : {"rows", "files_only", "count_only", "matches_only"}) {
        QJsonObject req{{QStringLiteral("pattern"), QStringLiteral("hidden_6188")}};
        if (qstrcmp(mode, "rows") != 0) req[QLatin1String(mode)] = true;
        const QJsonObject zero = search6188(root, req);
        EXPECT_EQ(zero.value(QStringLiteral("hidden_dirs_skipped")).toArray(),
                  QJsonArray{QStringLiteral(".github")}) << mode;
        EXPECT_TRUE(zero.value(QStringLiteral("hidden_dirs_hint")).toString()
                        .contains(QStringLiteral("include_hidden"))) << mode;
    }
    const QJsonObject found = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("plain_6188")}});
    EXPECT_FALSE(found.contains(QStringLiteral("hidden_dirs_skipped"))) << "a full answer needs no note";
    const QJsonObject capped = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("plain_6188")},
                                                 {QStringLiteral("max_results"), 1}});
    EXPECT_TRUE(capped.value(QStringLiteral("truncated")).toBool());
    EXPECT_EQ(capped.value(QStringLiteral("hidden_dirs_skipped")).toArray(),
              QJsonArray{QStringLiteral(".github")});
    const QJsonObject asked = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("hidden_6188")},
                                                {QStringLiteral("include_hidden"), true}});
    EXPECT_EQ(asked.value(QStringLiteral("matches")).toArray().size(), 1);
    EXPECT_FALSE(asked.contains(QStringLiteral("hidden_dirs_skipped")));
}

TEST(McpWorkspaceSearch, Ants6202BarInGlobIsNamed) {
    QTemporaryDir tmp;
    const QString root = hiddenFixture(tmp);
    const QJsonObject bar = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("plain_6188")},
                                              {QStringLiteral("glob"), QStringLiteral("*.txt|*.yml")}});
    EXPECT_TRUE(bar.value(QStringLiteral("glob_hint")).toString().contains(QStringLiteral("{a,b}")));
    const QJsonObject brace = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("plain_6188")},
                                                {QStringLiteral("glob"), QStringLiteral("*.{txt,yml}")}});
    EXPECT_FALSE(brace.contains(QStringLiteral("glob_hint")));
    EXPECT_EQ(brace.value(QStringLiteral("matches")).toArray().size(), 1);
}

// ANTS-4851 — a two-line shape is a normal thing to search for, and rg's error
// on one recommends a flag this verb must therefore actually offer.
TEST(McpWorkspaceSearch, Ants4851MultilineRegexCrossesALineBreak) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QFile f(root + QStringLiteral("/a.py"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("try:\n    run()\nexcept:\n    pass\n");
    f.close();
    const QJsonObject base{{QStringLiteral("pattern"), QStringLiteral("except:\\n\\s+pass")},
                           {QStringLiteral("regex"), true}};

    const QJsonObject refused = search6188(root, base);
    EXPECT_EQ(refused.value(QStringLiteral("code")).toString(), QStringLiteral("rg_failed"));
    EXPECT_TRUE(refused.value(QStringLiteral("hint")).toString().contains(QStringLiteral("multiline:true")))
        << QJsonDocument(refused).toJson().constData();

    QJsonObject req = base;
    req[QStringLiteral("multiline")] = true;
    const QJsonObject found = search6188(root, req);
    ASSERT_TRUE(found.value(QStringLiteral("ok")).toBool()) << QJsonDocument(found).toJson().constData();
    const QJsonArray rows = found.value(QStringLiteral("matches")).toArray();
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.at(0).toObject().value(QStringLiteral("line")).toInt(), 3);
    EXPECT_EQ(rows.at(0).toObject().value(QStringLiteral("text")).toString(), QStringLiteral("except: pass"));

    const QJsonObject literal = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("except:")},
                                                  {QStringLiteral("multiline"), true}});
    EXPECT_EQ(literal.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
    EXPECT_TRUE(literal.value(QStringLiteral("hint")).toString().contains(QStringLiteral("match_wrapped")));
}

TEST(McpWorkspaceSearch, Ants6207LookaroundRuns) {
    QTemporaryDir tmp;
    const QString root = hiddenFixture(tmp);
    const QJsonObject o = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("plain_6188(?= next)")},
                                            {QStringLiteral("regex"), true}});
    QProcess probe;
    probe.start(QStringLiteral("rg"), {QStringLiteral("--pcre2-version")});
    probe.waitForFinished(5000);
    if (probe.exitCode() == 0) {
        EXPECT_TRUE(o.value(QStringLiteral("ok")).toBool()) << QJsonDocument(o).toJson().constData();
        EXPECT_EQ(o.value(QStringLiteral("matches")).toArray().size(), 1);
    } else {
        EXPECT_EQ(o.value(QStringLiteral("code")).toString(), QStringLiteral("rg_failed"));
        EXPECT_TRUE(o.value(QStringLiteral("hint")).toString().contains(QStringLiteral("PCRE2")));
    }
}

// ANTS-5279 — context_before / context_after set each side on its own;
// `context` stays the shorthand that sets both.
namespace {
QString contextFixture(QTemporaryDir &tmp) {
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(root + QStringLiteral("/src"));
    QFile f(root + QStringLiteral("/src/sig.cpp"));
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("l1\nl2\nl3\nl4\nint target_5279(int x) {\nl6\nl7\nl8\nl9\n");
    return root;
}
QList<int> lines5279(const QJsonObject &m, const char *key) {
    QList<int> out;
    for (const QJsonValue &v : m.value(QLatin1String(key)).toArray())
        out << v.toObject().value(QStringLiteral("line")).toInt();
    return out;
}
} // namespace

TEST(McpWorkspaceSearch, Ants5279AsymmetricContext) {
    QTemporaryDir tmp;
    const QString root = contextFixture(tmp);
    const QJsonObject tail = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("target_5279")},
                                               {QStringLiteral("context_after"), 3}});
    const QJsonArray ms = tail.value(QStringLiteral("matches")).toArray();
    ASSERT_EQ(ms.size(), 1) << QJsonDocument(tail).toJson().toStdString();
    EXPECT_EQ(lines5279(ms.at(0).toObject(), "context_before"), QList<int>{});
    EXPECT_EQ(lines5279(ms.at(0).toObject(), "context_after"), (QList<int>{6, 7, 8}));

    const QJsonObject both = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("target_5279")},
                                               {QStringLiteral("context"), 2},
                                               {QStringLiteral("context_before"), 1}});
    const QJsonObject m = both.value(QStringLiteral("matches")).toArray().at(0).toObject();
    EXPECT_EQ(lines5279(m, "context_before"), QList<int>{4}) << "a side overrides `context`";
    EXPECT_EQ(lines5279(m, "context_after"), (QList<int>{6, 7}));
}

// ANTS-5278 — a bare-identifier search that pays for context or was cut off
// points at the call-site verbs; one that did neither carries no hint.
TEST(McpWorkspaceSearch, Ants5278BareIdentifierNamesCallSiteVerbs) {
    QTemporaryDir tmp;
    const QString root = contextFixture(tmp);
    const QJsonObject withCtx = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("target_5279")},
                                                  {QStringLiteral("context"), 2}});
    const QString hint = withCtx.value(QStringLiteral("call_site_hint")).toString();
    EXPECT_TRUE(hint.contains(QStringLiteral("find_caller")) && hint.contains(QStringLiteral("trace")))
        << QJsonDocument(withCtx).toJson().toStdString();

    const QJsonObject plain = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("target_5279")}});
    EXPECT_FALSE(plain.contains(QStringLiteral("call_site_hint"))) << "a cheap call pays nothing";

    const QJsonObject phrase = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("target_5279(int")},
                                                 {QStringLiteral("context"), 2}});
    EXPECT_FALSE(phrase.contains(QStringLiteral("call_site_hint"))) << "not a bare identifier";
}

// ANTS-4892 — a reply large enough to spill carries `files`, the
// [{file, count}] summary a caller narrows with, and the spill keeps it.
// A reply that will not spill pays nothing.
TEST(McpWorkspaceSearch, Ants4892SpilledSearchKeepsFileCounts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(root + QStringLiteral("/src"));
    for (int f = 0; f < 12; ++f) {
        QFile out(root + QStringLiteral("/src/f%1.txt").arg(f));
        ASSERT_TRUE(out.open(QIODevice::WriteOnly));
        const int lines = (f == 0) ? 30 : 5;   // one file dominates
        for (int i = 0; i < lines; ++i)
            out.write(QStringLiteral("needle_4892 file %1 row %2 padding padding\n")
                          .arg(f).arg(i).toUtf8());   // unique, so no row folds
    }
    QFile one(root + QStringLiteral("/src/once.txt"));
    ASSERT_TRUE(one.open(QIODevice::WriteOnly));
    one.write("solo_4892\n");
    one.close();

    mcp::setOffloadConfig(true, 4096, 2048);
    const QJsonObject big = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("needle_4892")},
                                              {QStringLiteral("max_results"), 200}});
    const QJsonArray files = big.value(QStringLiteral("files")).toArray();
    ASSERT_EQ(files.size(), 12) << QJsonDocument(big).toJson().toStdString().substr(0, 400);
    EXPECT_EQ(files.at(0).toObject().value(QStringLiteral("file")).toString().toStdString(),
              "src/f0.txt") << "the most-matched file comes first";
    EXPECT_EQ(files.at(0).toObject().value(QStringLiteral("count")).toInt(), 30);

    const QJsonObject spilled = QJsonDocument::fromJson(
        mcp::offloadBody(QStringLiteral("workspace_search"),
                         QString::fromUtf8(QJsonDocument(big).toJson(QJsonDocument::Compact)))
            .toUtf8()).object();
    ASSERT_TRUE(spilled.value(QStringLiteral("offloaded")).toBool())
        << "offload did not fire — nothing was being tested";
    EXPECT_EQ(spilled.value(QStringLiteral("files")).toArray().size(), 12)
        << "the summary must survive the spill";

    const QJsonObject small = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("solo_4892")}});
    EXPECT_FALSE(small.contains(QStringLiteral("files"))) << "a reply that will not spill pays nothing";
    mcp::setOffloadConfig(false, 16384, 2048);
}

// ANTS-5588 — when the context lines are what would make a reply spill, the
// reply drops them, keeps every row inline and says `context_dropped`. A reply
// that fits keeps its context.
TEST(McpWorkspaceSearch, Ants5588ContextIsDroppedBeforeRows) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(root + QStringLiteral("/src"));
    {
        QFile out(root + QStringLiteral("/src/rows.txt"));
        ASSERT_TRUE(out.open(QIODevice::WriteOnly));
        for (int i = 0; i < 100; ++i)   // a needle every fifth line: 20 rows
            out.write((i % 5 == 2
                           ? QStringLiteral("needle_5588 row %1\n").arg(i)
                           : QStringLiteral("filler %1 padding padding padding\n").arg(i))
                          .toUtf8());
        QFile one(root + QStringLiteral("/src/once.txt"));
        ASSERT_TRUE(one.open(QIODevice::WriteOnly));
        one.write("before\nsolo_5588\nafter\n");
    }
    const auto bytes = [](const QJsonObject &o) {
        return QJsonDocument(o).toJson(QJsonDocument::Compact).size();
    };

    mcp::setOffloadConfig(true, 4096, 2048);
    const QJsonObject bare = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("needle_5588")}});
    ASSERT_EQ(bare.value(QStringLiteral("matches")).toArray().size(), 20);
    ASSERT_LT(bytes(bare), 4096) << "fixture: the rows alone must fit inline";

    const QJsonObject wide = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("needle_5588")},
                                               {QStringLiteral("context"), 2}});
    const QJsonArray rows = wide.value(QStringLiteral("matches")).toArray();
    EXPECT_EQ(rows.size(), 20) << "every row is kept";
    ASSERT_TRUE(wide.value(QStringLiteral("context_dropped")).isBool());
    EXPECT_TRUE(wide.value(QStringLiteral("context_dropped")).toBool());
    for (const QJsonValue &v : rows) {
        EXPECT_FALSE(v.toObject().contains(QStringLiteral("context_before")));
        EXPECT_FALSE(v.toObject().contains(QStringLiteral("context_after")));
    }
    EXPECT_LT(bytes(wide), 4096) << "the reply now stays inline";
    EXPECT_FALSE(wide.contains(QStringLiteral("files"))) << "it will not spill, so no spill summary";

    const QJsonObject small = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("solo_5588")},
                                                {QStringLiteral("context"), 1}});
    const QJsonObject hit = small.value(QStringLiteral("matches")).toArray().at(0).toObject();
    EXPECT_TRUE(hit.contains(QStringLiteral("context_before"))) << "a reply that fits keeps its context";
    EXPECT_FALSE(small.contains(QStringLiteral("context_dropped")));
    mcp::setOffloadConfig(false, 16384, 2048);
}

// ANTS-5348 — `last_per_file: N` returns each file's LAST N matches, in file
// order, with every matched file's full count in `file_totals`. A row cap that
// lands inside a file keeps that file's latest rows, not its earliest.
TEST(McpWorkspaceSearch, Ants5348LastMatchesPerFile) {
    QTemporaryDir tmp;
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QDir().mkpath(root + QStringLiteral("/docs"));
    {
        QFile f(root + QStringLiteral("/docs/log.md"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("loop_5348 one\nloop_5348 two\nloop_5348 three\nloop_5348 four\n");
    }
    const auto lineList = [](const QJsonArray &ms) {
        QList<int> out;
        for (const QJsonValue &v : ms) out << v.toObject().value(QStringLiteral("line")).toInt();
        return out;
    };
    const QJsonObject r = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("loop_5348")},
                                            {QStringLiteral("last_per_file"), 2}});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().toStdString();
    EXPECT_EQ(lineList(r.value(QStringLiteral("matches")).toArray()), (QList<int>{3, 4}));
    EXPECT_FALSE(r.value(QStringLiteral("truncated")).toBool())
        << "rows dropped by last_per_file are the request, not a truncation";
    const QJsonArray totals = r.value(QStringLiteral("file_totals")).toArray();
    ASSERT_EQ(totals.size(), 1);
    EXPECT_EQ(totals.at(0).toObject().value(QStringLiteral("file")).toString(),
              QStringLiteral("docs/log.md"));
    EXPECT_EQ(totals.at(0).toObject().value(QStringLiteral("count")).toInt(), 4);

    // The row cap inside the file keeps the newest row.
    const QJsonObject capped = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("loop_5348")},
                                                 {QStringLiteral("last_per_file"), 2},
                                                 {QStringLiteral("max_results"), 1}});
    EXPECT_EQ(lineList(capped.value(QStringLiteral("matches")).toArray()), QList<int>{4});
    EXPECT_TRUE(capped.value(QStringLiteral("truncated")).toBool());

    // Paging from the front contradicts reading from the end.
    const QJsonObject paged = search6188(root, {{QStringLiteral("pattern"), QStringLiteral("loop_5348")},
                                                {QStringLiteral("last_per_file"), 2},
                                                {QStringLiteral("offset"), 1}});
    EXPECT_EQ(paged.value(QStringLiteral("code")).toString(), QStringLiteral("bad_args"));
}

// ANTS-5705 — a file the project marks as generated is left out of a search,
// the reply names what was left out, and the caller can still reach it.
TEST(McpWorkspaceSearch, Ants5705GeneratedFilesAreSkipped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    const auto write = [&](const char *rel, const char *text) {
        QDir().mkpath(QFileInfo(root + QLatin1String(rel)).path());
        QFile f(root + QLatin1String(rel));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(text);
    };
    write("/.git/config", "");
    write("/ROADMAP.md", "needle_5705 rendered\n");
    write("/notes.md", "needle_5705 written\n");
    write("/.ants/project.json", "{\"generated\":[\"ROADMAP.md\"]}");
    const auto files = [](const QJsonObject &o) {
        QStringList out;
        for (const QJsonValue &v : o.value(QStringLiteral("matches")).toArray())
            out << v.toObject().value(QStringLiteral("file")).toString();
        out.sort();
        return out;
    };
    const QJsonObject pattern{{QStringLiteral("pattern"), QStringLiteral("needle_5705")}};

    const QJsonObject plain = search6188(root, pattern);
    ASSERT_TRUE(plain.value(QStringLiteral("ok")).toBool()) << QJsonDocument(plain).toJson().toStdString();
    EXPECT_EQ(files(plain), QStringList{QStringLiteral("notes.md")});
    EXPECT_EQ(plain.value(QStringLiteral("generated_skipped")).toArray(),
              QJsonArray{QStringLiteral("ROADMAP.md")});

    QJsonObject all = pattern;
    all[QStringLiteral("include_generated")] = true;
    const QJsonObject asked = search6188(root, all);
    EXPECT_EQ(files(asked), (QStringList{QStringLiteral("ROADMAP.md"), QStringLiteral("notes.md")}));
    EXPECT_FALSE(asked.contains(QStringLiteral("generated_skipped")));

    // A glob that names the generated file is a request for it.
    QJsonObject named = pattern;
    named[QStringLiteral("glob")] = QStringLiteral("ROADMAP.md");
    EXPECT_EQ(files(search6188(root, named)), QStringList{QStringLiteral("ROADMAP.md")});

    // A wider glob is not.
    QJsonObject wide = pattern;
    wide[QStringLiteral("glob")] = QStringLiteral("*.md");
    EXPECT_EQ(files(search6188(root, wide)), QStringList{QStringLiteral("notes.md")});
}
