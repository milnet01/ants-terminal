// Source-grep + runtime harness for ANTS-1249 — locks the wiring
// contract for the new file_outline MCP tool. See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "../../_support/expect.h"
#include "fileoutline.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

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

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}



}  // namespace

TEST(McpFileOutline, WiringContract) {
    expect_reset();

    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string ciHdr = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_H_PATH);
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    const std::string foCppPath =
        std::string(ANTS_SOURCE_DIR) + "/src/fileoutline.cpp";
    const std::string foCpp = ants_test::slurpFile(foCppPath.c_str());

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
                                              reqPos + 6000);
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

    EXPECT_EQ(0, expect_failures()) << expect_failures() << " ANTS-1249 wiring invariant(s) failed";
}

// INV-9 — runtime smoke. compute() against the in-tree
// src/auditdialog.cpp must return at least 8 symbols. This catches
// regex-set regressions that the source-grep above would miss
// (e.g. a regex that compiles but matches nothing).
TEST(McpFileOutline, RuntimeFloor) {
    expect_reset();
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

// INV-11 (ANTS-2028) — free functions with a single-token return type
// must be captured. `rxCppFunc` previously folded the return type and
// the name into one possessive `[\w:<>&*\s]++` class, leaving nothing
// for the `\s++(\w+)` name capture, so a free function like
// `int alpha()` never surfaced — only class/struct/namespace and
// qualified `Class::method` forms did. This silently narrowed
// file_outline + read_region symbol mode coverage.
TEST(McpFileOutline, FreeFunctionCapture) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/free.cpp");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(
            "int alpha() {\n"                                  // free func, 1-token return
            "    return 0;\n"
            "}\n"
            "static QByteArray slurpBody(const char *p) {\n"   // static + ptr arg
            "    return {};\n"
            "}\n"
            "const std::string &makeName(int n);\n"           // qualified return + ref, decl
            "void Widget::method() {\n"                        // qualified member (rxCppMember)
            "}\n"
            "int beta() {\n"                                  // real free func — must surface
            "    return gamma(7);\n"                          // ANTS-2147: call in return position — NOT a symbol
            "}\n");
        f.close();
    }
    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Cpp, /*includeDocComment=*/false,
        /*maxSymbols=*/100);
    ASSERT_TRUE(out.value("ok").toBool());
    const QJsonArray symbols = out.value("symbols").toArray();
    auto hasName = [&](const char *n) {
        for (const auto &v : symbols)
            if (v.toObject().value("name").toString() == QLatin1String(n))
                return true;
        return false;
    };
    EXPECT_TRUE(hasName("alpha"))
        << "ANTS-2028: free function 'int alpha()' not captured";
    EXPECT_TRUE(hasName("slurpBody"))
        << "ANTS-2028: free function 'static QByteArray slurpBody(...)' not captured";
    EXPECT_TRUE(hasName("makeName"))
        << "ANTS-2028: free-function declaration "
           "'const std::string &makeName(int);' not captured";
    // Regression guard: the qualified member still resolves via rxCppMember.
    EXPECT_TRUE(hasName("Widget::method"))
        << "ANTS-2028: qualified member 'Widget::method' regressed";
    // ANTS-2147 — a statement-position call (`return gamma(7);`) must not be
    // emitted as a function symbol; the real enclosing `beta` still surfaces.
    EXPECT_TRUE(hasName("beta"))
        << "ANTS-2147: free function 'int beta()' not captured";
    EXPECT_FALSE(hasName("gamma"))
        << "ANTS-2147: return-position call 'return gamma(7);' "
           "mis-detected as a function symbol";
}

// ANTS-2148 follow-up (DOOM_Ants feedback 2026-06-26) — a C/C++ function whose
// PARAMETER LIST wraps across source lines (id-Software / K&R prototypes, e.g.
// DOOM's emit_wall) must still be outlined, AND resolve to the header's START
// line so read_region symbol-mode returns the full definition. Pre-fix,
// rxCppFunc / rxCppFuncOpen required the ')' on the opening line, so these defs
// were silently dropped (symbol_not_found on read_region).
TEST(McpFileOutline, MultiLineSignatureCapture) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/mesh.c");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(
            "void push_vert(int x) { }\n"                    // L1 single-line guard
            "static void emit_wall(int bld, int seg,\n"     // L2 header start, wrapped
            "                      int texnum, int flags,\n" // L3 continuation
            "                      int topplane)\n"          // L4 ')' here, '{' next line
            "{\n"                                            // L5
            "    return;\n"                                  // L6
            "}\n"                                            // L7
            "static int clip_poly(int a,\n"                  // L8 header start, wrapped
            "                     int c) {\n"                // L9 ')' + '{' same line
            "    return 0;\n"                                // L10
            "}\n");                                          // L11
        f.close();
    }
    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Cpp, /*includeDocComment=*/false,
        /*maxSymbols=*/100);
    ASSERT_TRUE(out.value("ok").toBool());
    const QJsonArray symbols = out.value("symbols").toArray();
    auto lineOf = [&](const char *n) -> int {
        for (const auto &v : symbols)
            if (v.toObject().value("name").toString() == QLatin1String(n))
                return v.toObject().value("line").toInt();
        return -1;
    };
    EXPECT_EQ(lineOf("push_vert"), 1)
        << "single-line function regressed";
    EXPECT_EQ(lineOf("emit_wall"), 2)
        << "ANTS-2148: wrapped-signature 'emit_wall' not captured at its header "
           "start line (read_region symbol-mode would miss the signature)";
    EXPECT_EQ(lineOf("clip_poly"), 8)
        << "ANTS-2148: wrapped-signature 'clip_poly' (brace on close line) not "
           "captured at its header start line";
    bool hasReturn = false;
    for (const auto &v : symbols)
        if (v.toObject().value("name").toString() == QLatin1String("return"))
            hasReturn = true;
    EXPECT_FALSE(hasReturn) << "interior 'return' mis-detected as a symbol";
}

// ANTS-2150 — brace-family generic outline: auto-detection by extension
// yields the precise language name (rust/go/typescript) and extracts symbols.
TEST(McpFileOutline, BraceFamilyGenericOutline) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    auto outlineOf = [&](const char *rel, const char *body) {
        const QString path = tmp.path() + QLatin1Char('/') + QLatin1String(rel);
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(body);
        f.close();
        // Mode::Auto so pickModeByExt routes to Mode::Generic by extension.
        return FileOutline::compute(path, FileOutline::Mode::Auto,
                                    /*includeDocComment=*/false,
                                    /*maxSymbols=*/100);
    };
    auto hasName = [](const QJsonObject &o, const char *n) {
        for (const auto &v : o.value("symbols").toArray())
            if (v.toObject().value("name").toString() == QLatin1String(n))
                return true;
        return false;
    };

    const QJsonObject rs = outlineOf("lib.rs",
        "pub fn rustParse(p: &str) -> u32 { 0 }\n"
        "pub struct RustCfg { x: u32 }\n");
    EXPECT_EQ(rs.value("language").toString().toStdString(), "rust");
    EXPECT_TRUE(hasName(rs, "rustParse")) << "rust fn not outlined";
    EXPECT_TRUE(hasName(rs, "RustCfg"))   << "rust struct not outlined";

    const QJsonObject go = outlineOf("server.go",
        "func goNewServer(addr string) *Server {\n}\n"
        "type Server struct {\n}\n");
    EXPECT_EQ(go.value("language").toString().toStdString(), "go");
    EXPECT_TRUE(hasName(go, "goNewServer")) << "go func not outlined";
    EXPECT_TRUE(hasName(go, "Server"))      << "go type not outlined";

    const QJsonObject ts = outlineOf("service.ts",
        "export class TsService {\n}\n"
        "export const tsHandler = (req, res) => {\n}\n");
    EXPECT_EQ(ts.value("language").toString().toStdString(), "typescript");
    EXPECT_TRUE(hasName(ts, "TsService")) << "ts class not outlined";
    EXPECT_TRUE(hasName(ts, "tsHandler")) << "ts arrow assignment not outlined";
}

// INV-10 — non-existent path returns the not_found code without
// crashing.
TEST(McpFileOutline, NotFoundPath) {
    expect_reset();
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
