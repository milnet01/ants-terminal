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

#include <QString>
#include <QStringList>
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

TEST(McpSubsystem, WiringContract) {
    expect_reset();

    const std::string ciCpp  = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr  = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr  = slurp(SRC_RC_HEADER);
    const std::string rcCpp  = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp  = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string smCpp  = slurp(SRC_SUBSYSTEMMAP_CPP_PATH);
    const std::string smHdr  = slurp(SRC_SUBSYSTEMMAP_H_PATH);
    const std::string cmake  = slurp(CMAKELISTS_PATH);

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

    // INV-12 — current CLAUDE.md parses to >= 15 unique lanes
    // including "vtparser" (spec § 10 step 2 floor).
    SubsystemMap::clearCacheForTests();
    const QString claudeMdPath = QString::fromUtf8(ANTS_CLAUDE_MD_PATH);
    const QVector<SubsystemMap::Lane> lanes =
        SubsystemMap::cachedLanes(claudeMdPath);
    bool haveVtparser = false;
    for (const auto &l : lanes) {
        if (l.name == QStringLiteral("vtparser")) { haveVtparser = true; break; }
    }
    char detail12[200];
    std::snprintf(detail12, sizeof detail12,
                  "parsed %lld lanes from CLAUDE.md, expected >=15 "
                  "including \"vtparser\" (haveVtparser=%s)",
                  static_cast<long long>(lanes.size()),
                  haveVtparser ? "true" : "false");
    expect(lanes.size() >= 15 && haveVtparser, "INV-12", detail12);

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1251 invariant(s) failed";
}
