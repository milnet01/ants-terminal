// Source-grep + runtime harness for ANTS-1249 — locks the wiring
// contract for the new file_outline MCP tool. See spec.md.
//
// Exit 0 = all 10 invariants hold.

#include "../../_support/expect.h"
#include "fileoutline.h"

#include <cstdio>
#include <regex>
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
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
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
    const std::string rcCpp = ants_test::slurpRemoteControl();
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
    // ANTS-2223 — `path` is no longer strictly required: the multi-path
    // `paths` form satisfies the verb without it. The schema must register
    // `paths` + `etags` props instead.
    {
        const size_t reqPos = ciCpp.find("\"file_outline\"");
        bool ok = false;
        if (reqPos != std::string::npos) {
            // The window exists only to keep this assertion inside
            // file_outline's OWN schema block rather than matching some later
            // tool's props. It is a bound, not a contract — so when a genuine
            // schema addition pushes `paths`/`etags` past it, WIDEN IT. Trimming
            // the schema to fit a test's magic number would be the tail wagging
            // the dog. Widened 6000 → 9000 by ANTS-3800, which added `generic`
            // and `glsl` to the mode enum; this class of break has bitten
            // repeatedly and reads as a real wiring failure every time.
            // Widened 9000 → 12000 by ANTS-4384/4365, which added the `sizes`
            // and `raw` props ahead of the props[] assignments.
            const size_t windowEnd = std::min(ciCpp.size(),
                                              reqPos + 12000);
            const std::string window = ciCpp.substr(reqPos,
                                                    windowEnd - reqPos);
            ok = contains(window, "\"paths\"") &&
                 contains(window, "\"etags\"");
        }
        expect(ok, "INV-5 multipath",
               "file_outline inputSchema does not register the ANTS-2223 "
               "\"paths\" + \"etags\" props");
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

// ANTS-2223 — multi-path (`paths:[...]`) wiring. The branch lives in the
// cmdFileOutline handler, which needs a live MainWindow to invoke, so the
// contract is locked at the source level (mirrors INV-2/INV-4 above). Three
// load-bearing facts: the handler branches on a `paths` array, both call-sites
// share the extracted `outlineOneFile` helper, and the optional `etags` map
// 304s an unchanged entry to an `unchanged` stub.
TEST(McpFileOutline, MultiPathWiring) {
    expect_reset();
    const std::string rcCpp =
        ants_test::slurpRemoteControl();

    // The shared per-file helper is extracted and called from both forms.
    expect(contains(rcCpp, "outlineOneFile("),
           "ANTS-2223 helper",
           "remotecontrol.cpp missing the extracted outlineOneFile() helper");
    // Multi-path branch keys on a `paths` array.
    expect(contains(rcCpp, "QStringLiteral(\"paths\")") &&
           contains(rcCpp, "pathsVal.isArray()"),
           "ANTS-2223 branch",
           "cmdFileOutline does not branch on a `paths` array");
    // Per-file etag + 304 stub via the optional `etags` map.
    expect(contains(rcCpp, "outlineFileEtag(") &&
           contains(rcCpp, "QStringLiteral(\"etags\")") &&
           contains(rcCpp, "\"unchanged\""),
           "ANTS-2223 per-file-304",
           "cmdFileOutline multi-path is missing the per-file etag / "
           "`etags` 304 / unchanged-stub path");
    // The batch envelope carries files[] + count.
    expect(contains(rcCpp, "out[\"files\"]") &&
           contains(rcCpp, "out[\"count\"]"),
           "ANTS-2223 envelope",
           "cmdFileOutline multi-path does not emit files[] + count");

    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-2223 multi-path wiring invariant(s) failed";
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

// ANTS-3399 (Vestige feedback 2026-06-30) — a C++ method whose return type is
// itself namespace-qualified (`JPH::BodyID Class::method(...)`) must outline as
// the bare qualified member name, NOT with the return type glued on. The old
// heuristic located the space before the FIRST `::`, which fell inside the
// qualified return type, so read_region symbol-mode couldn't resolve it.
TEST(McpFileOutline, QualifiedReturnTypeMemberName) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/phys.cpp");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(
            "bool PhysicsWorld::initialize() {\n"                        // unqualified return
            "    return true;\n"
            "}\n"
            "JPH::BodyID PhysicsWorld::createStaticBody(int shape) {\n"  // qualified return
            "    return {};\n"
            "}\n"
            "JPH::BodyInterface PhysicsWorld::getBodyInterface() {\n"    // qualified return
            "    return {};\n"
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
    EXPECT_TRUE(hasName("PhysicsWorld::initialize"))
        << "ANTS-3399: unqualified-return member regressed";
    EXPECT_TRUE(hasName("PhysicsWorld::createStaticBody"))
        << "ANTS-3399: qualified-return member glued the return type onto the name";
    EXPECT_TRUE(hasName("PhysicsWorld::getBodyInterface"))
        << "ANTS-3399: second qualified-return member not cleanly named";
    // The return type must NOT survive in any symbol name.
    for (const auto &v : symbols) {
        const QString nm = v.toObject().value("name").toString();
        EXPECT_FALSE(nm.startsWith(QLatin1String("JPH::")))
            << "ANTS-3399: return type leaked into symbol name: "
            << nm.toStdString();
    }
}

// ANTS-3404 (Album Builder feedback 2026-06-30) — the Python outliner must emit
// class methods qualified as `Class.method` (indented `def`), not only
// top-level classes/functions, so read_region symbol-mode can address a method.
TEST(McpFileOutline, PythonClassMethodQualified) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/play_queue.py");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write(
            "class RepeatMode:\n"
            "    pass\n"
            "\n"
            "class PlayQueue:\n"
            "    def __init__(self):\n"
            "        self._i = 0\n"
            "    def next(self):\n"
            "        return self._i\n"
            "\n"
            "def module_level():\n"
            "    return 1\n");
        f.close();
    }
    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Py, /*includeDocComment=*/false,
        /*maxSymbols=*/100);
    ASSERT_TRUE(out.value("ok").toBool());
    const QJsonArray symbols = out.value("symbols").toArray();
    auto hasName = [&](const char *n) {
        for (const auto &v : symbols)
            if (v.toObject().value("name").toString() == QLatin1String(n))
                return true;
        return false;
    };
    EXPECT_TRUE(hasName("RepeatMode")) << "ANTS-3404: top-level class regressed";
    EXPECT_TRUE(hasName("PlayQueue")) << "ANTS-3404: top-level class regressed";
    EXPECT_TRUE(hasName("PlayQueue.next"))
        << "ANTS-3404: class method not emitted as Class.method";
    EXPECT_TRUE(hasName("PlayQueue.__init__"))
        << "ANTS-3404: dunder method not qualified";
    EXPECT_TRUE(hasName("module_level"))
        << "ANTS-3404: top-level function regressed (should stay bare)";
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

    // Ruby (ANTS-2150) — folds into Mode::Generic: def/class/module are in the
    // keyword alternation; the optional `self.` receiver captures singleton
    // methods, and the optional trailing `?`/`!` captures predicate/bang names.
    const QJsonObject rb = outlineOf("thing.rb",
        "module RubyMod\n"
        "  class RubyThing\n"
        "    def ruby_instance\n"
        "    end\n"
        "    def self.ruby_singleton\n"
        "    end\n"
        "    def valid?\n"
        "    end\n"
        "  end\n"
        "end\n");
    EXPECT_EQ(rb.value("language").toString().toStdString(), "ruby");
    EXPECT_TRUE(hasName(rb, "RubyMod"))        << "ruby module not outlined";
    EXPECT_TRUE(hasName(rb, "RubyThing"))      << "ruby class not outlined";
    EXPECT_TRUE(hasName(rb, "ruby_instance"))  << "ruby def not outlined";
    EXPECT_TRUE(hasName(rb, "ruby_singleton")) << "ruby `def self.` singleton not outlined";
    EXPECT_TRUE(hasName(rb, "valid?"))         << "ruby predicate-method name not outlined";
}

// ANTS-4090 — a top-level `const NAME = …` that is NOT an arrow function was
// invisible to the generic scanner: rxGenericDecl lists `const` only as a
// MODIFIER before a declaration keyword, and rxGenericArrow requires `=>`. In a
// file that stores payloads in template literals those bindings are the largest
// regions, so the outline showed a hole exactly where the content is (reporter:
// stats.mjs, 1006 lines, the outline jumping 685 → 933 across a 95-line CSS
// literal and a 110-line client-script one).
TEST(McpFileOutline, Ants4090TopLevelBindings) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QString path = tmp.path() + QStringLiteral("/stats.mjs");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(
        "import { readFile } from 'node:fs/promises';\n"
        "const CSS = `\n"
        "  body { color: red; }\n"
        "`;\n"
        "export const CLIENT_JS = `\n"
        "  console.log(1);\n"
        "`;\n"
        "let mutableTotal = 0;\n"
        "const SQL = 'SELECT id, name, created_at FROM runs ORDER BY id DESC';\n"
        "export const handler = (req) => {\n"
        "  const localOnly = compute();\n"
        "  return localOnly;\n"
        "};\n"
        "function sortRows(a) { return a; }\n");
    f.close();

    const QJsonObject js = FileOutline::compute(path, FileOutline::Mode::Auto,
                                                /*includeDocComment=*/false,
                                                /*maxSymbols=*/100);
    auto sym = [&](const char *n) {
        for (const auto &v : js.value("symbols").toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value("name").toString() == QLatin1String(n)) return o;
        }
        return QJsonObject();
    };

    EXPECT_EQ(js.value("language").toString().toStdString(), "javascript");
    EXPECT_FALSE(sym("CSS").isEmpty())
        << "a top-level const holding a template literal must be outlined";
    EXPECT_FALSE(sym("CLIENT_JS").isEmpty()) << "`export const` must be outlined";
    EXPECT_FALSE(sym("mutableTotal").isEmpty()) << "top-level `let` must be outlined";

    // The existing rules keep precedence: an arrow assignment is still a func,
    // and a function declaration is untouched.
    EXPECT_EQ(sym("handler").value("kind").toString().toStdString(), "func")
        << "the arrow rule must still win over the new binding rule";
    EXPECT_FALSE(sym("sortRows").isEmpty());

    // Its own kind, so a caller can tell a data binding from a function.
    EXPECT_EQ(sym("CSS").value("kind").toString().toStdString(), "const");

    // Indentation is the top-level discriminator: a local inside a function
    // body must not reach the outline, or every JS file becomes noise.
    EXPECT_TRUE(sym("localOnly").isEmpty())
        << "an indented local binding must stay out of the outline";

    // The signature stops at the assignment — otherwise a one-line payload
    // (or the head of a 95-line literal) lands in every outline response.
    const QString sqlSig = sym("SQL").value("signature").toString();
    EXPECT_TRUE(sqlSig.contains(QStringLiteral("SQL")))
        << "signature must still name the binding (got: " << sqlSig.toStdString() << ")";
    EXPECT_FALSE(sqlSig.contains(QStringLiteral("SELECT")))
        << "signature must be truncated at the assignment, not carry the value";
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

// ANTS-4384 — opt-in per-symbol `bytes` / `lines`.
//
// The two questions that motivate outlining a large document before
// restructuring it — which section carries the weight, and where is the
// natural seam — could not be answered from the reply, though the server had
// already walked every line. A caller outlining a 39,722-byte SKILL.md got 20
// headings with line numbers and no sizes, and fell back to `awk`.
//
// The md rule is the part that matters: a `##` section's extent runs to the
// next heading at the SAME OR HIGHER level, so it INCLUDES its `###`
// children. Scoped to the next symbol regardless of level, the reply answers
// "how long is this paragraph" rather than "where is the seam".
TEST(McpFileOutline, Ants4384SizesAreOptInAndLevelScoped) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/doc.md");

    // Fixture with exact, hand-countable extents.
    //  L1 `# Top`            level 1 → to EOF          (lines 1..12)
    //  L3 `## Alpha`         level 2 → next `##` at L9 (lines 3..8)
    //  L5 `### Alpha child`  level 3 → next `##` at L9 (lines 5..8)
    //  L9 `## Omega`         level 2 → EOF             (lines 9..12)
    const QByteArray body =
        "# Top\n"            // 1
        "\n"                 // 2
        "## Alpha\n"         // 3
        "\n"                 // 4
        "### Alpha child\n"  // 5
        "aaa\n"              // 6
        "aaa\n"              // 7
        "\n"                 // 8
        "## Omega\n"         // 9
        "z\n"                // 10
        "z\n"                // 11
        "z\n";               // 12
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }

    auto sizeOf = [](const QJsonObject &out, const char *name,
                     const char *field) -> int {
        for (const auto &v : out.value("symbols").toArray()) {
            const QJsonObject s = v.toObject();
            if (s.value("name").toString() == QLatin1String(name))
                return s.value(field).toInt(-1);
        }
        return -1;
    };
    auto hasField = [](const QJsonObject &out, const char *field) {
        for (const auto &v : out.value("symbols").toArray())
            if (v.toObject().contains(QLatin1String(field))) return true;
        return false;
    };

    // Opt-in: the default envelope is unchanged, so no existing caller pays.
    const QJsonObject plain = FileOutline::compute(
        path, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        /*maxSymbols=*/100, /*withSizes=*/false);
    EXPECT_FALSE(hasField(plain, "bytes"))
        << "sizes must be opt-in — the default reply stays byte-identical";
    EXPECT_FALSE(hasField(plain, "lines"));

    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        /*maxSymbols=*/100, /*withSizes=*/true);
    ASSERT_TRUE(out.value("ok").toBool());

    EXPECT_EQ(sizeOf(out, "Top", "lines"), 12)
        << "the level-1 heading spans the whole document";
    EXPECT_EQ(sizeOf(out, "Alpha", "lines"), 6)
        << "a `##` section runs to the next `##`, INCLUDING its `###` child — "
           "scoping to the next symbol of any level would give 2";
    EXPECT_EQ(sizeOf(out, "Alpha child", "lines"), 4)
        << "the `###` child runs to the next same-or-higher heading";
    EXPECT_EQ(sizeOf(out, "Omega", "lines"), 4)
        << "the last symbol runs to EOF";

    // Bytes agree with the line spans, and with the file total.
    EXPECT_EQ(sizeOf(out, "Top", "bytes"),
              static_cast<int>(body.size()));
    EXPECT_EQ(sizeOf(out, "Alpha", "bytes"),
              sizeOf(out, "Alpha child", "bytes") +
                  static_cast<int>(qstrlen("## Alpha\n\n")));
}

// ANTS-4384 — the flat (non-md) modes are sibling-scoped: every symbol is at
// the same level, so each runs to the next one. A file whose symbols are
// emitted out of line order (a typedef-struct emits at its START line once the
// brace balances, i.e. AFTER later symbols were appended) must still get
// extents in document order rather than array order.
TEST(McpFileOutline, Ants4384FlatModeIsSiblingScopedInDocumentOrder) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/a.c");
    const QByteArray body =
        "typedef struct Tag {\n"   // 1
        "    int x;\n"             // 2
        "} Alias;\n"               // 3
        "\n"                       // 4
        "int later(void) {\n"      // 5
        "    return 0;\n"          // 6
        "}\n";                     // 7
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        /*maxSymbols=*/100, /*withSizes=*/true);
    ASSERT_TRUE(out.value("ok").toBool());

    // Every emitted symbol carries a positive, in-range extent — the property
    // that fails outright if extents are computed in array order over a set
    // emitted out of document order (a later symbol would yield a negative or
    // whole-file span).
    int checked = 0;
    for (const auto &v : out.value("symbols").toArray()) {
        const QJsonObject s = v.toObject();
        if (!s.contains(QStringLiteral("lines"))) continue;
        ++checked;
        const int startLine = s.value("line").toInt();
        const int nLines    = s.value("lines").toInt();
        EXPECT_GT(nLines, 0) << s.value("name").toString().toStdString();
        EXPECT_LE(startLine + nLines - 1, out.value("total_lines").toInt())
            << "extent runs past EOF for "
            << s.value("name").toString().toStdString();
        EXPECT_GT(s.value("bytes").toInt(), 0);
    }
    EXPECT_GT(checked, 0) << "fixture produced no sized symbols";
}

// ANTS-4349 — the batch form reported top-level ok:true when EVERY path
// failed, so a caller branching on the documented success signal read a total
// miss as a success and reasoned about an empty symbol set. ok:true stays (a
// partial hit IS a success, and a batch verb that refuses on any miss is
// unusable for "outline whatever exists") — what was missing is the count a
// caller can actually branch on.
TEST(McpFileOutline, Ants4349BatchReportsFoundAndMissingCounts) {
    // slurpRemoteControl(), not a per-TU path define: rc_tu_split INV-11
    // requires every `src/remotecontrol_*.cpp` literal in CMakeLists.txt to
    // live inside the ANTS_RC_SOURCES_REL block, and a private path define is
    // exactly the drift it guards against.
    const std::string rc = ants_test::slurpRemoteControl();
    EXPECT_TRUE(contains(rc, "files_found"))
        << "the paths[] envelope must say how many paths resolved";
    EXPECT_TRUE(contains(rc, "files_missing"))
        << "…and how many did not, so ok:true is not the only signal";
}

// ANTS-4379 — a Python MODULE-LEVEL constant is a symbol.
//
// `_MISPARSE` and `_E_TOTALS_MISMATCH` were reported "no definition found"
// while both are plain top-level assignments in the very file the doc cited.
// The brace family has carried a `const` kind since ANTS-4090; Python had no
// equivalent, and doc_symbols shares this index — so every such citation read
// as a broken reference.
//
// The shape is unavoidable rather than exotic: a codebase whose user-facing
// error strings are module constants is the natural Python idiom, and exactly
// what a spec ABOUT error messages must cite. The cost was dilution — 19
// unresolved of 60, only 2 of this class, but all 19 hand-triaged, which is
// how a genuinely unresolved symbol gets waved through as "probably another
// false positive".
TEST(McpFileOutline, Ants4379PythonModuleConstants) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/mod.py");
    const QByteArray body =
        "_MISPARSE = \"could not parse row {n}\"\n"
        "_E_TOTALS_MISMATCH: str = \"totals do not match\"\n"
        "\n"
        "class Importer:\n"
        "    LIMIT = 10\n"          // class attribute — NOT a module constant
        "    def run(self):\n"
        "        local_tmp = 1\n"   // a local — must never surface
        "        return local_tmp\n"
        "\n"
        "AFTER_CLASS = 3\n"         // top level again, after the class body
        "\n"
        "def helper():\n"
        "    pass\n";
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        /*maxSymbols=*/100);
    ASSERT_TRUE(out.value("ok").toBool());

    auto kindOf = [&](const char *n) -> QString {
        for (const auto &v : out.value("symbols").toArray()) {
            const QJsonObject s = v.toObject();
            if (s.value("name").toString() == QLatin1String(n))
                return s.value("kind").toString();
        }
        return QString();
    };

    EXPECT_EQ(kindOf("_MISPARSE"), QStringLiteral("const"));
    EXPECT_EQ(kindOf("_E_TOTALS_MISMATCH"), QStringLiteral("const"))
        << "an annotated assignment is still a module constant";
    EXPECT_EQ(kindOf("AFTER_CLASS"), QStringLiteral("const"))
        << "a top-level assignment AFTER a class body is a module constant";

    // Indented assignments must NOT surface. Emitting them would bury the
    // outline in every intermediate variable in every function body — the
    // opposite of what this verb is for.
    EXPECT_TRUE(kindOf("LIMIT").isEmpty())
        << "a class attribute is not a module constant";
    EXPECT_TRUE(kindOf("local_tmp").isEmpty())
        << "a function local must never surface";

    // The shapes that already worked still work.
    EXPECT_EQ(kindOf("Importer"), QStringLiteral("class"));
    EXPECT_EQ(kindOf("helper"), QStringLiteral("func"));
    EXPECT_EQ(kindOf("Importer.run"), QStringLiteral("func"))
        << "class-method qualification (ANTS-3404) must be unaffected";
}

// ANTS-4361 — a single self-contained HTML page gets an outline.
//
// `file_outline` on an 828-line template.html returned language:"unknown"
// with no symbols array at all, so learning where things were before editing
// seven regions cost a native Read of all 828 lines (~10k tokens) — the
// verb's own 13-39× saving forgone on the LARGEST file in the project. Not
// niche: a single self-contained page is the normal shape for a small local
// tool, and several projects filing feedback here are web front-ends.
//
// The irony the reporter named is the fix: the JS inside a <script> IS the
// brace family this outliner already parses well, and only the extension ever
// routed it to the fallback.
TEST(McpFileOutline, Ants4361HtmlLandmarksAndScriptBodies) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/page.html");
    const QByteArray body =
        "<!doctype html>\n"
        "<html>\n"
        "<head>\n"
        "<style>\n"
        "  #main { color: red; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<div id=\"toolbar\">x</div>\n"
        "<section id='results'></section>\n"
        "<script type=\"application/json\">\n"
        "  {\"notAFunction\": 1}\n"
        "</script>\n"
        "<script>\n"
        "const API_BASE = \"/api/v1\";\n"
        "function renderResults(rows) { return rows; }\n"
        "</script>\n"
        "</body>\n"
        "</html>\n";
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    const QJsonObject out = FileOutline::compute(
        path, FileOutline::Mode::Auto, /*includeDocComment=*/false,
        /*maxSymbols=*/100);
    ASSERT_TRUE(out.value("ok").toBool());
    EXPECT_EQ(out.value("language").toString(), QStringLiteral("html"))
        << "the extension must route to the HTML mode, not to \"unknown\"";

    auto kindOf = [&](const char *n) -> QString {
        for (const auto &v : out.value("symbols").toArray()) {
            const QJsonObject s = v.toObject();
            if (s.value("name").toString() == QLatin1String(n))
                return s.value("kind").toString();
        }
        return QString();
    };
    auto lineOf = [&](const char *n) -> int {
        for (const auto &v : out.value("symbols").toArray()) {
            const QJsonObject s = v.toObject();
            if (s.value("name").toString() == QLatin1String(n))
                return s.value("line").toInt();
        }
        return -1;
    };

    // Landmarks — the regions read_region can then fetch on their own.
    EXPECT_EQ(kindOf("<style>"), QStringLiteral("region"));
    EXPECT_EQ(kindOf("<script>"), QStringLiteral("region"));

    // Anchors — every element carrying an id=, in both quote styles.
    EXPECT_EQ(kindOf("toolbar"), QStringLiteral("anchor"));
    EXPECT_EQ(kindOf("results"), QStringLiteral("anchor"))
        << "single-quoted id= is as common in a hand-written page";

    // The valuable half: the brace-family parser over the <script> body.
    EXPECT_EQ(kindOf("renderResults"), QStringLiteral("func"));
    EXPECT_EQ(kindOf("API_BASE"), QStringLiteral("const"))
        << "ANTS-4090's top-level const kind comes along for free";
    EXPECT_GT(lineOf("renderResults"), lineOf("<script>"))
        << "script symbols report their real line in the FILE, which is what "
           "makes them addressable";

    // A non-JavaScript <script> must NOT be handed to the brace parser — its
    // contents are data, and a JSON blob would emit noise as symbols.
    EXPECT_TRUE(kindOf("notAFunction").isEmpty())
        << "type=\"application/json\" is data, not code";

    // The CSS id selector inside <style> must not be mistaken for an anchor:
    // only an id= ATTRIBUTE is one.
    EXPECT_TRUE(kindOf("main").isEmpty());
}

// ANTS-4396 — md heading-depth filter.
//
// Outlining a 532-line append-only feedback log returned ~85 symbols
// dominated by `###` finding titles that are themselves full sentences, when
// the question — "what is still open?" — needed only the `##` day headings.
// ~4-5k tokens to orient on a file whose useful surface was about six lines,
// recurring every session and monotonically worse as the file only grows.
//
// `max_symbols` was not a substitute and made it worse: it truncates from the
// TOP, so on an append-only log it keeps the OLDEST entries and drops the
// newest — the opposite of what such a file wants.
TEST(McpFileOutline, Ants4396MdHeadingDepthFilter) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/log.md");
    const QByteArray body =
        "# Feedback\n"
        "\n"
        "## 2026-08-01\n"
        "\n"
        "### An older finding whose title is a whole sentence\n"
        "\n"
        "### Another older finding, likewise\n"
        "\n"
        "## 2026-08-14\n"
        "\n"
        "### Today's finding\n";
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(body);
    }
    auto names = [](const QJsonObject &o) {
        QStringList n;
        for (const auto &v : o.value("symbols").toArray())
            n << v.toObject().value("name").toString();
        return n;
    };

    // No filter — every level, the behaviour that must not change.
    const QJsonObject all = FileOutline::compute(
        path, FileOutline::Mode::Auto, false, 100, false, /*maxHeadingLevel=*/0);
    EXPECT_EQ(names(all).size(), 6);

    // Depth 2 — only `#` and `##`.
    const QJsonObject shallow = FileOutline::compute(
        path, FileOutline::Mode::Auto, false, 100, false, /*maxHeadingLevel=*/2);
    const QStringList got = names(shallow);
    EXPECT_EQ(got, (QStringList{QStringLiteral("Feedback"),
                                QStringLiteral("2026-08-01"),
                                QStringLiteral("2026-08-14")}))
        << "got: " << got.join(QStringLiteral(", ")).toStdString();

    // The filter must free BUDGET, not merely hide rows: with max_symbols:2
    // and the filter on, the two day headings both survive. Filtering as a
    // post-pass would spend the budget on the `###` noise first and return
    // only the first day — which is the same failure the caller was already
    // hitting with max_symbols alone.
    const QJsonObject capped = FileOutline::compute(
        path, FileOutline::Mode::Auto, false, /*maxSymbols=*/3, false, 2);
    EXPECT_TRUE(names(capped).contains(QStringLiteral("2026-08-14")))
        << "the newest day heading must survive a tight cap once the deeper "
           "headings are filtered out";

    // Out-of-range means "no filter", not "no headings" — a typo must not
    // return an empty outline that reads as a file with no structure.
    EXPECT_EQ(names(FileOutline::compute(path, FileOutline::Mode::Auto,
                                         false, 100, false, 99)).size(), 6);
}
