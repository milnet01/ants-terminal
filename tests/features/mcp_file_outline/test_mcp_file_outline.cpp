// Source-grep + runtime harness for ANTS-1249 — locks the wiring
// contract for the new file_outline MCP tool. See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "fileoutline.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

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
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
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

int g_failures = 0;

void expect(bool ok, const char *label, const char *detail = nullptr) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s%s%s\n",
                     label,
                     detail ? " — " : "",
                     detail ? detail : "");
    }
}

}  // namespace

TEST(McpFileOutline, WiringContract) {
    g_failures = 0;

    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr = slurp(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);

    const std::string foCppPath =
        std::string(ANTS_SOURCE_DIR) + "/src/fileoutline.cpp";
    const std::string foCpp = slurp(foCppPath.c_str());

    // INV-1 — cmdFileOutline declared public.
    expect(contains(rcHdr, "cmdFileOutline(const QJsonObject &req)"),
           "INV-1",
           "cmdFileOutline decl missing from src/remotecontrol.h");

    // INV-2 — at least the three INV anchors that fire in
    // remotecontrol.cpp (INV-1, INV-2, INV-10).
    std::regex anchorRe(R"(//\s*ANTS-1249-INV-\d+)");
    auto begin = std::sregex_iterator(rcCpp.begin(), rcCpp.end(), anchorRe);
    auto end   = std::sregex_iterator();
    const long rcAnchors = std::distance(begin, end);
    char detail2[160];
    std::snprintf(detail2, sizeof detail2,
                  "expected >=3 // ANTS-1249-INV-N anchors in "
                  "remotecontrol.cpp (cmdFileOutline body), found %ld",
                  rcAnchors);
    expect(rcAnchors >= 3, "INV-2", detail2);

    // INV-3 — six regex builders + caps in fileoutline.cpp.
    const char *regexBuilders[] = {
        "rxCppMember", "rxCppType", "rxCppFunc",
        "rxCppQt",     "rxPy",      "rxMdHeading",
    };
    for (const char *b : regexBuilders) {
        char label[64];
        std::snprintf(label, sizeof label, "INV-3 %s", b);
        char d[160];
        std::snprintf(d, sizeof d,
                      "fileoutline.cpp missing static regex builder %s",
                      b);
        expect(contains(foCpp, b), label, d);
    }
    expect(contains(foCpp, "kMaxLineBytes"),
           "INV-3 line-cap",
           "fileoutline.cpp missing kMaxLineBytes constant (catastrophic-backtracking guard)");
    expect(contains(foCpp, "kHeaderDocByteCap"),
           "INV-3 header-cap",
           "fileoutline.cpp missing kHeaderDocByteCap constant (header_doc 2 KiB cap)");
    expect(contains(foCpp, ".optimize()"),
           "INV-3 optimize",
           "fileoutline.cpp does not call QRegularExpression::optimize() — JIT warm-up not enforced");

    // INV-4 — IPC dispatcher routes "file-outline".
    expect(contains(rcCpp, "\"file-outline\"") &&
           contains(rcCpp, "cmdFileOutline"),
           "INV-4",
           "remotecontrol.cpp dispatch missing \"file-outline\" → cmdFileOutline routing");

    // INV-5 — tools/list registers "file_outline" with the right
    // schema properties.
    expect(contains(ciCpp, "\"file_outline\""),
           "INV-5 name",
           "tools/list missing \"file_outline\" name registration");
    const char *requiredProps[] = {
        "\"path\"", "\"mode\"", "\"include_doc_comment\"", "\"max_symbols\"",
    };
    for (const char *p : requiredProps) {
        char label[64];
        std::snprintf(label, sizeof label, "INV-5 prop %s", p);
        char d[160];
        std::snprintf(d, sizeof d,
                      "claudeintegration.cpp does not register %s in "
                      "file_outline inputSchema.properties",
                      p);
        expect(contains(ciCpp, p), label, d);
    }
    {
        const size_t reqPos = ciCpp.find("\"file_outline\"");
        bool ok = false;
        if (reqPos != std::string::npos) {
            const size_t windowEnd = std::min(ciCpp.size(),
                                              reqPos + 4000);
            const std::string window = ciCpp.substr(reqPos,
                                                    windowEnd - reqPos);
            ok = contains(window, "\"required\"") &&
                 contains(window, "\"path\"");
        }
        expect(ok, "INV-5 required",
               "file_outline inputSchema does not declare [\"path\"] as required");
    }

    // INV-6 — tools/list schema declares the file_outline tool.
    // ANTS-1253 collapsed the per-tool dispatch into a registry lookup.
    std::regex schemaRe(R"("name"\]\s*=\s*"file_outline")");
    expect(std::regex_search(ciCpp, schemaRe),
           "INV-6",
           "claudeintegration.cpp missing tools/list schema entry for file_outline");

    // INV-7 — header has the single registry surface (ANTS-1253).
    expect(contains(ciHdr, "registerToolProvider(const QString &name"),
           "INV-7a",
           "claudeintegration.h missing registerToolProvider declaration (ANTS-1253)");
    expect(contains(ciHdr, "m_toolProviders"),
           "INV-7b",
           "claudeintegration.h missing m_toolProviders registry member (ANTS-1253)");

    // INV-8 — mainwindow.cpp registers file_outline via the registry.
    expect(contains(mwCpp, "registerToolProvider(\"file_outline\""),
           "INV-8a",
           "mainwindow.cpp does not register file_outline in setupClaudeMcpProviders (ANTS-1253)");
    expect(contains(mwCpp, "cmdFileOutline"),
           "INV-8b",
           "mainwindow.cpp does not delegate the provider lambda to cmdFileOutline");

    if (g_failures) {
        FAIL() << g_failures << " ANTS-1249 wiring invariant(s) failed";
    }
}

// INV-9 — runtime smoke. compute() against the in-tree
// src/auditdialog.cpp must return at least 8 symbols. This catches
// regex-set regressions that the source-grep above would miss
// (e.g. a regex that compiles but matches nothing).
TEST(McpFileOutline, RuntimeFloor) {
    const QString auditPath =
        QString::fromUtf8(ANTS_SOURCE_DIR) + "/src/auditdialog.cpp";
    const QJsonObject out = FileOutline::compute(
        auditPath, FileOutline::Mode::Auto,
        /*includeDocComment=*/true,
        /*maxSymbols=*/500);
    ASSERT_TRUE(out.value("ok").toBool())
        << "INV-9 prereq — compute(auditdialog.cpp) returned ok:false";
    const QJsonArray symbols = out.value("symbols").toArray();
    EXPECT_GE(symbols.size(), 8)
        << "INV-9 — fewer than 8 symbols found in auditdialog.cpp "
        << "(actual=" << symbols.size() << "); regex set has regressed";
    // Sanity: language is "cpp" via the auto-pick path.
    EXPECT_EQ(out.value("language").toString().toStdString(), "cpp");
}

// INV-10 — non-existent path returns the not_found code without
// crashing.
TEST(McpFileOutline, NotFoundPath) {
    const QString missing =
        QString::fromUtf8(ANTS_SOURCE_DIR) +
        "/src/this-file-does-not-exist-1249.cpp";
    const QJsonObject out = FileOutline::compute(
        missing, FileOutline::Mode::Cpp,
        /*includeDocComment=*/true,
        /*maxSymbols=*/100);
    EXPECT_FALSE(out.value("ok").toBool());
    EXPECT_EQ(out.value("code").toString().toStdString(), "not_found");
}
