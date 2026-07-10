// Source-grep harness for ANTS-1251 — locks the wiring contract for
// the new `subsystem` consolidated MCP tool (map / files /
// recent_changes). See spec.md.
//
// Exit 0 = all 12 invariants hold.

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include "../../_support/expect.h"
#include "subsystemmap.h"

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
#ifndef SRC_SUBSYSTEMMAP_CPP_PATH
#error "SRC_SUBSYSTEMMAP_CPP_PATH compile definition required"
#endif
#ifndef SRC_SUBSYSTEMMAP_H_PATH
#error "SRC_SUBSYSTEMMAP_H_PATH compile definition required"
#endif
#ifndef ANTS_CLAUDE_MD_PATH
#error "ANTS_CLAUDE_MD_PATH compile definition required"
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

TEST(McpSubsystem, WiringContract) {
    expect_reset();

    const std::string ciCpp  = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr  = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr  = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp  = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp  = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string smCpp  = ants_test::slurpFile(SRC_SUBSYSTEMMAP_CPP_PATH);
    const std::string smHdr  = ants_test::slurpFile(SRC_SUBSYSTEMMAP_H_PATH);
    const std::string cmake  = ants_test::slurpFile(CMAKELISTS_PATH);

    // INV-1 — cmdSubsystem declared public on RemoteControl.
    expect(contains(rcHdr, "cmdSubsystem(const QJsonObject &req)"),
           "INV-1",
           "cmdSubsystem decl missing from src/remotecontrol.h "
           "(must sit next to cmdGitState in the public block)");

    // INV-2 — body has >= 7 INV anchors across remotecontrol.cpp +
    // subsystemmap.{cpp,h}.
    std::regex anchorRe(R"(//\s*ANTS-1251-INV-\d+)");
    auto countAnchors = [&](const std::string &s) -> long {
        auto begin = std::sregex_iterator(s.begin(), s.end(), anchorRe);
        auto end   = std::sregex_iterator();
        return std::distance(begin, end);
    };
    const long anchorCount = countAnchors(rcCpp) + countAnchors(smCpp) +
                             countAnchors(smHdr);
    char detail2[200];
    std::snprintf(detail2, sizeof detail2,
                  "expected >=7 // ANTS-1251-INV-N anchors across "
                  "remotecontrol.cpp + subsystemmap.{cpp,h}, found %ld",
                  anchorCount);
    expect(anchorCount >= 7, "INV-2", detail2);

    // INV-3 — IPC dispatcher routes "subsystem".
    expect(contains(rcCpp, "\"subsystem\"") &&
           contains(rcCpp, "cmdSubsystem"),
           "INV-3",
           "remotecontrol.cpp dispatch missing \"subsystem\" → "
           "cmdSubsystem routing");

    // INV-4 — tools/list registers a single "subsystem" entry with
    // op enum {map, files, recent_changes} and op in required[].
    expect(contains(ciCpp, "\"subsystem\""),
           "INV-4a",
           "tools/list missing \"subsystem\" name registration");
    {
        const size_t pos = ciCpp.find("ssTool[\"name\"] = \"subsystem\"");
        bool ok = false;
        if (pos != std::string::npos) {
            const size_t windowEnd = std::min(ciCpp.size(), pos + 4000);
            const std::string window = ciCpp.substr(pos, windowEnd - pos);
            ok = contains(window, "\"map\"") &&
                 contains(window, "\"files\"") &&
                 contains(window, "\"recent_changes\"") &&
                 contains(window, "\"required\"") &&
                 contains(window, "\"op\"");
        }
        expect(ok, "INV-4b",
               "subsystem inputSchema does not declare op enum "
               "{map, files, recent_changes} + op in required[]");
    }

    // INV-5 — tools/list schema declares the subsystem tool.
    // ANTS-1253 collapsed the per-tool dispatch into a registry lookup.
    std::regex schemaRe(R"("name"\]\s*=\s*"subsystem")");
    expect(std::regex_search(ciCpp, schemaRe),
           "INV-5",
           "claudeintegration.cpp missing tools/list schema entry for subsystem");

    // INV-6 — header has the single registry surface (ANTS-1253).
    expect(contains(ciHdr, "registerToolProvider(const QString &name"),
           "INV-6a",
           "claudeintegration.h missing registerToolProvider declaration (ANTS-1253)");
    expect(contains(ciHdr, "m_toolProviders"),
           "INV-6b",
           "claudeintegration.h missing m_toolProviders registry member (ANTS-1253)");

    // INV-7 — mainwindow.cpp registers subsystem via the registry.
    expect(contains(mwCpp, "registerToolProvider(\"subsystem\""),
           "INV-7a",
           "mainwindow.cpp does not register subsystem in "
           "setupClaudeMcpProviders (ANTS-1253)");
    expect(contains(mwCpp, "cmdSubsystem"),
           "INV-7b",
           "mainwindow.cpp does not delegate the provider lambda to "
           "m_remoteControl->cmdSubsystem");

    // INV-8 — cmdSubsystem dispatches on op ∈ {map, files,
    // recent_changes} and surfaces both error codes.
    expect(contains(rcCpp, "\"map\"") &&
           contains(rcCpp, "\"files\"") &&
           contains(rcCpp, "\"recent_changes\""),
           "INV-8a",
           "remotecontrol.cpp does not dispatch on op string literals "
           "{map, files, recent_changes}");
    expect(contains(rcCpp, "\"bad_op\"") &&
           contains(rcCpp, "\"unknown_lane\""),
           "INV-8b",
           "remotecontrol.cpp does not surface {bad_op, unknown_lane} "
           "error codes (ANTS-1251 § 5 trailing paragraph)");

    // INV-9 — cmdSubsystem composes cmdGitState for recent_changes.
    {
        const size_t pos = rcCpp.find("RemoteControl::cmdSubsystem");
        bool ok = false;
        if (pos != std::string::npos) {
            const std::string window = rcCpp.substr(pos);
            ok = contains(window, "cmdGitState");
        }
        expect(ok, "INV-9",
               "cmdSubsystem body does not compose cmdGitState for "
               "the recent_changes op (spec § 3 / INV-5)");
    }

    // INV-10 — subsystemmap.h exposes Lane / parse / cachedLanes.
    expect(contains(smHdr, "namespace SubsystemMap") &&
           contains(smHdr, "struct Lane") &&
           contains(smHdr, "parse(") &&
           contains(smHdr, "cachedLanes("),
           "INV-10",
           "subsystemmap.h surface missing Lane / parse / cachedLanes");

    // INV-11 — CMake wires src/subsystemmap.cpp into ants_core_lib.
    expect(contains(cmake, "src/subsystemmap.cpp"),
           "INV-11",
           "CMakeLists.txt does not list src/subsystemmap.cpp in "
           "ants_core_lib SOURCES");

    // INV-12 — the resolved module-map source parses to >= 15 unique
    // lanes including "vtparser" (spec § 10 step 2 floor). Post-ANTS-1292
    // the source is docs/subsystems.md; resolveSource() finds it.
    SubsystemMap::clearCacheForTests();
    const QString claudeMdPath = QString::fromUtf8(ANTS_CLAUDE_MD_PATH);
    const QString resolvedSrc  = SubsystemMap::resolveSource(claudeMdPath);
    const QVector<SubsystemMap::Lane> lanes =
        SubsystemMap::cachedLanes(resolvedSrc);
    bool haveVtparser = false;
    for (const auto &l : lanes) {
        if (l.name == QStringLiteral("vtparser")) { haveVtparser = true; break; }
    }
    char detail12[200];
    std::snprintf(detail12, sizeof detail12,
                  "parsed %lld lanes from resolved source, expected >=15 "
                  "including \"vtparser\" (haveVtparser=%s)",
                  static_cast<long long>(lanes.size()),
                  haveVtparser ? "true" : "false");
    expect(lanes.size() >= 15 && haveVtparser, "INV-12", detail12);

    // ANTS-1292 INV-13 — resolveSource() prefers docs/subsystems.md when
    // present (the canonical home), not CLAUDE.md.
    expect(resolvedSrc.endsWith(QStringLiteral("docs/subsystems.md")),
           "ANTS-1292/INV-13",
           "resolveSource(CLAUDE.md) did not resolve to docs/subsystems.md");

    // ANTS-1292 INV-14 — the catalogue moved OUT of CLAUDE.md: parsing the
    // (stub) CLAUDE.md directly yields no lanes, so it no longer bloats the
    // session preamble.
    SubsystemMap::clearCacheForTests();
    const QVector<SubsystemMap::Lane> claudeMdLanes =
        SubsystemMap::cachedLanes(claudeMdPath);
    char detail14[160];
    std::snprintf(detail14, sizeof detail14,
                  "CLAUDE.md still parses to %lld lanes; the module map "
                  "should be a stub pointing at docs/subsystems.md",
                  static_cast<long long>(claudeMdLanes.size()));
    expect(claudeMdLanes.isEmpty(), "ANTS-1292/INV-14", detail14);

    // ANTS-1292 INV-2 — empty input → empty output (no crash, no
    // fabricated path).
    expect(SubsystemMap::resolveSource(QString()).isEmpty(),
           "ANTS-1292/INV-2",
           "resolveSource(\"\") must return empty, not a fabricated path");

    // ANTS-1292 INV-3 — un-migrated project (no sibling docs/subsystems.md)
    // falls back to the given CLAUDE.md path unchanged. Use a path whose
    // directory has no docs/subsystems.md sibling.
    const QString unmigrated =
        QStringLiteral("/nonexistent-ants-1292-probe/CLAUDE.md");
    expect(SubsystemMap::resolveSource(unmigrated) == unmigrated,
           "ANTS-1292/INV-3",
           "resolveSource must return the CLAUDE.md path unchanged when no "
           "docs/subsystems.md sibling exists (back-compat fallback)");

    // ANTS-3414 — op:map honours an optional `name` substring filter.
    // The schema declares a `name` property (so it is no longer flagged in
    // ignored_args), and the op:map branch filters lanesJson by
    // name.contains(nameFilter, Qt::CaseInsensitive) and echoes `name`.
    expect(contains(ciCpp, "props[\"name\"] = nameProp"),
           "ANTS-3414/schema",
           "subsystem inputSchema does not declare the op:map `name` prop");
    {
        const size_t pos = rcCpp.find("RemoteControl::cmdSubsystem");
        bool ok = false;
        if (pos != std::string::npos) {
            const std::string window = rcCpp.substr(pos);
            ok = contains(window, "req.value(\"name\")") &&
                 contains(window, "Qt::CaseInsensitive");
        }
        expect(ok, "ANTS-3414/handler",
               "cmdSubsystem op:map does not apply the case-insensitive "
               "`name` substring filter");
    }

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1251/ANTS-1292 invariant(s) failed";
}

// ANTS-1796 — a backticked library-name qualifier inside a parenthetical
// (e.g. ``- `auditautofix` (Qt6::Core, `ants_audit_lib`) — ...``) must NOT
// be harvested as a separate subsystem lane. The docs/subsystems.md format
// contract says qualifier-paren forms are *tolerated* (ignored), not parsed
// for names. Pre-fix, parse() harvested every backticked token in the
// prefix, so `subsystem op=map` emitted bogus lanes named after CMake
// library targets (ants_audit_lib / ants_core_lib / ants_dialogs_lib).
TEST(McpSubsystem, ParenQualifierLibNameNotHarvested) {
    expect_reset();

    const QString body = QStringLiteral(
        "## Module map (src/)\n"
        "\n"
        "- `auditautofix` (Qt6::Core, `ants_audit_lib`) — native safe-list "
        "auto-fixer.\n"
        "- `llmclient` (Qt6::Core+Network, `ants_core_lib`) — streaming chat "
        "client.\n"
        "- `luaengine` / `pluginmanager` — sandboxed Lua 5.4.\n");

    const QVector<SubsystemMap::Lane> lanes = SubsystemMap::parse(body);

    bool sawLibQualifier = false;
    bool sawAutofix = false, sawLlm = false, sawLua = false, sawPlugin = false;
    for (const auto &l : lanes) {
        if (l.name == QStringLiteral("ants_audit_lib") ||
            l.name == QStringLiteral("ants_core_lib") ||
            l.name == QStringLiteral("ants_dialogs_lib")) {
            sawLibQualifier = true;
        }
        if (l.name == QStringLiteral("auditautofix"))   sawAutofix = true;
        if (l.name == QStringLiteral("llmclient"))      sawLlm = true;
        if (l.name == QStringLiteral("luaengine"))      sawLua = true;
        if (l.name == QStringLiteral("pluginmanager"))  sawPlugin = true;
    }

    expect(!sawLibQualifier, "ANTS-1796/no-lib-qualifier",
           "parse() harvested a backticked library-name qualifier inside "
           "parens as a subsystem lane (must be ignored)");
    // The legitimate names — including both halves of a ` / `-joined
    // multi-name bullet — must survive.
    expect(sawAutofix && sawLlm && sawLua && sawPlugin,
           "ANTS-1796/legit-names-kept",
           "parse() dropped a legitimate subsystem name while stripping "
           "paren qualifiers");

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-1796 invariant(s) failed";
}

// ANTS-3481 — sourceHasModuleMap distinguishes "heading present" from
// "heading absent", so indie_review_orchestrate can report an empty partition
// with an honest cause (module_map_unparseable vs no_lanes).
TEST(McpSubsystem, SourceHasModuleMapDetectsHeading) {
    expect_reset();
    SubsystemMap::clearCacheForTests();

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto put = [&](const QString &rel, const QByteArray &body) -> QString {
        const QString p = dir.path() + QLatin1Char('/') + rel;
        QFile f(p);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body);
        f.close();
        return p;
    };

    // A finbreak-style CLAUDE.md: HAS `## Module map` but as a `- path —
    // description` list the lane parser can't harvest → present, 0 lanes.
    const QString finbreak = put(QStringLiteral("CLAUDE.md"),
        "# finbreak\n\n## Module map\n\n"
        "- src/api/main.py — FastAPI entry point.\n"
        "- src/filter/rules.py — rule engine.\n");
    expect(SubsystemMap::sourceHasModuleMap(finbreak),
           "ANTS-3481/present",
           "sourceHasModuleMap must return true when the heading exists");
    expect(SubsystemMap::cachedLanes(finbreak).isEmpty(),
           "ANTS-3481/unparseable",
           "the path-list bullets must derive zero lanes (the repro)");

    // A doc with NO module-map heading → absent.
    const QString noMap = put(QStringLiteral("nomap.md"),
        "# proj\n\n## Build\n\nsome text.\n");
    expect(!SubsystemMap::sourceHasModuleMap(noMap),
           "ANTS-3481/absent",
           "sourceHasModuleMap must return false with no heading");
    // A non-existent path → false, no crash.
    expect(!SubsystemMap::sourceHasModuleMap(dir.path() + "/nope.md"),
           "ANTS-3481/missing-file",
           "sourceHasModuleMap must return false for a missing file");

    // Wiring: cmdIndieReviewOrchestrate branches on sourceHasModuleMap and
    // emits the distinct module_map_unparseable code.
    const std::string rc = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    expect(rc.find("module_map_unparseable") != std::string::npos,
           "ANTS-3481/wiring-code",
           "cmdIndieReviewOrchestrate must emit module_map_unparseable");
    expect(rc.find("SubsystemMap::sourceHasModuleMap") != std::string::npos,
           "ANTS-3481/wiring-call",
           "orchestrate must call SubsystemMap::sourceHasModuleMap");

    EXPECT_EQ(0, expect_failures()) << expect_failures()
        << " ANTS-3481 invariant(s) failed";
}
