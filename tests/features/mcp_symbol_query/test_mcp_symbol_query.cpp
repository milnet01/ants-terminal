// ANTS-1303 — feature-conformance test for find_definition /
// find_caller. Live SymbolQuery behaviour against a synthetic source
// tree + source-grep wiring contract. See spec.md.

#include "../../_support/expect.h"

#include "symbolquery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

void writeFile(const QString &root, const QString &rel, const QString &body) {
    const QString full = root + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile f(full);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "setup-fail: cannot write %s\n",
                     full.toUtf8().constData());
        std::exit(2);
    }
    f.write(body.toUtf8());
    f.close();
}

// Does any DefMatch sit at this project-relative file with this kind?
bool hasDef(const SymbolQuery::DefResult &r, const QString &file,
            const QString &kind) {
    for (const auto &d : r.definitions)
        if (d.file == file && d.kind == kind) return true;
    return false;
}

bool hasCaller(const SymbolQuery::CallResult &r, const QString &file) {
    for (const auto &c : r.callers)
        if (c.file == file) return true;
    return false;
}

}  // namespace

TEST(McpSymbolQuery, LiveBehaviour) {
    expect_reset();

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // --- C++ ---------------------------------------------------------
    writeFile(root, QStringLiteral("src/widget.h"),
              QStringLiteral("class Widget {\n"
                             "public:\n"
                             "    void doThing();\n"   // declaration (; )
                             "};\n"));
    writeFile(root, QStringLiteral("src/widget.cpp"),
              QStringLiteral("#include \"widget.h\"\n"
                             "void Widget::doThing() {\n"   // definition ({)
                             "    helper();\n"
                             "}\n"
                             "void Widget::other() {\n"
                             "    widget.doThing();\n"       // caller (. )
                             "    obj->doThing();\n"         // caller (->)
                             "}\n"));

    // --- ANTS-1700: qualified call sites must not be defs -------------
    writeFile(root, QStringLiteral("src/calls.cpp"),
              QStringLiteral("#include \"widget.h\"\n"
                             "QByteArray ns::slurpBody(const char *p) {\n"  // real qualified def
                             "    return {};\n"
                             "}\n"
                             "void user() {\n"
                             "    ns::slurpBody(\"x\");\n"        // qualified call — NOT a def
                             "    auto y = ns::slurpBody(p);\n"   // qualified call in expr
                             "}\n"
                             "QByteArray retCaller(const char *p) {\n"
                             "    return slurpBody(p);\n"         // ANTS-2146: return-position call — NOT a def
                             "}\n"));

    // --- Python ------------------------------------------------------
    writeFile(root, QStringLiteral("app.py"),
              QStringLiteral("def compute(x):\n"
                             "    return x + 1\n"
                             "\n"
                             "def run():\n"
                             "    compute(5)\n"));

    // --- Lua ---------------------------------------------------------
    writeFile(root, QStringLiteral("mod.lua"),
              QStringLiteral("function M.greet(name)\n"
                             "  return name\n"
                             "end\n"
                             "local function caller()\n"
                             "  greet(\"x\")\n"
                             "end\n"));

    // --- Shell -------------------------------------------------------
    writeFile(root, QStringLiteral("script.sh"),
              QStringLiteral("deploy() {\n"
                             "  echo hi\n"
                             "}\n"
                             "main() {\n"
                             "  deploy\n"
                             "}\n"));

    // --- Skipped dirs ------------------------------------------------
    writeFile(root, QStringLiteral("build/skipme.cpp"),
              QStringLiteral("void Widget::skipMe() {\n}\n"));
    writeFile(root, QStringLiteral("node_modules/dep.cpp"),
              QStringLiteral("void Widget::skipMe() {\n}\n"));
    writeFile(root, QStringLiteral(".hidden/secret.cpp"),
              QStringLiteral("void Widget::skipMe() {\n}\n"));

    SymbolQuery::Options def;  // all-defaults (auto, def caps)

    // INV-1 — definition ordering + kind.
    const auto d = SymbolQuery::findDefinition(root, QStringLiteral("doThing"), def);
    expect(d.ok, "INV-1: findDefinition ok");
    expect(hasDef(d, QStringLiteral("src/widget.cpp"), QStringLiteral("definition")),
           "INV-1: widget.cpp doThing is a definition");
    expect(hasDef(d, QStringLiteral("src/widget.h"), QStringLiteral("declaration")),
           "INV-1: widget.h doThing is a declaration");
    expect(!d.definitions.isEmpty() &&
               d.definitions.first().kind == QStringLiteral("definition"),
           "INV-1: definitions ordered definition-first");

    // INV-6 — project-relative paths (no temp-root prefix, no leading /).
    bool relOk = true;
    for (const auto &m : d.definitions)
        if (m.file.startsWith(QLatin1Char('/')) || m.file.contains(root))
            relOk = false;
    expect(relOk, "INV-6: definition file paths are project-relative");

    // INV-2 — callers exclude the def line; definition attached.
    const auto c = SymbolQuery::findCaller(root, QStringLiteral("doThing"), def);
    expect(c.ok, "INV-2: findCaller ok");
    expect(hasCaller(c, QStringLiteral("src/widget.cpp")),
           "INV-2: widget.cpp has a doThing caller");
    bool noDefLineInCallers = true;
    for (const auto &cm : c.callers)
        if (cm.context.contains(QStringLiteral("void Widget::doThing")))
            noDefLineInCallers = false;
    expect(noDefLineInCallers, "INV-2: definition line not reported as caller");
    expect(c.definition.has_value() &&
               c.definition->kind == QStringLiteral("definition"),
           "INV-2: best definition attached to find_caller");

    // INV-3 — per-language anchors fire.
    expect(SymbolQuery::findDefinition(root, QStringLiteral("compute"), def)
               .definitions.size() >= 1, "INV-3: python def found");
    expect(SymbolQuery::findDefinition(root, QStringLiteral("greet"), def)
               .definitions.size() >= 1, "INV-3: lua def found");
    expect(SymbolQuery::findDefinition(root, QStringLiteral("deploy"), def)
               .definitions.size() >= 1, "INV-3: shell def found");
    expect(SymbolQuery::findCaller(root, QStringLiteral("compute"), def)
               .callers.size() >= 1, "INV-3: python caller found");
    expect(SymbolQuery::findCaller(root, QStringLiteral("deploy"), def)
               .callers.size() >= 1, "INV-3: shell caller found");

    // INV-4 — build* / node_modules / dot-dirs skipped.
    const auto skip = SymbolQuery::findDefinition(root, QStringLiteral("skipMe"), def);
    expect(skip.definitions.isEmpty(),
           "INV-4: symbol only in skipped dirs is not found");

    // INV-5 — explicit lang filter restricts the scan.
    SymbolQuery::Options pyOnly;
    pyOnly.lang = SymbolQuery::Lang::Py;
    expect(SymbolQuery::findDefinition(root, QStringLiteral("doThing"), pyOnly)
               .definitions.isEmpty(),
           "INV-5: lang=py finds no C++ symbol");
    expect(SymbolQuery::findDefinition(root, QStringLiteral("compute"), pyOnly)
               .definitions.size() >= 1,
           "INV-5: lang=py still finds the python def");

    // INV-7 — maxResults caps; *_count is pre-cap; truncated flips.
    SymbolQuery::Options cap1;
    cap1.maxResults = 1;
    const auto capped = SymbolQuery::findCaller(root, QStringLiteral("doThing"), cap1);
    expect(capped.callers.size() == 1, "INV-7: callers capped to maxResults");
    expect(capped.callersTotal == 2, "INV-7: callersTotal is pre-cap (2)");
    expect(capped.truncated, "INV-7: truncated flag set when capped");

    // INV-8 — isValidSymbol accept/reject.
    expect(SymbolQuery::isValidSymbol(QStringLiteral("Foo_bar1")),
           "INV-8: valid identifier accepted");
    expect(SymbolQuery::isValidSymbol(QStringLiteral("_x")),
           "INV-8: leading underscore accepted");
    expect(!SymbolQuery::isValidSymbol(QString()), "INV-8: empty rejected");
    expect(!SymbolQuery::isValidSymbol(QStringLiteral("1abc")),
           "INV-8: leading digit rejected");
    expect(!SymbolQuery::isValidSymbol(QStringLiteral("a.b")),
           "INV-8: dot rejected");
    expect(!SymbolQuery::isValidSymbol(QStringLiteral("a(b")),
           "INV-8: paren (regex metachar) rejected");
    expect(!SymbolQuery::isValidSymbol(QString(129, QLatin1Char('a'))),
           "INV-8: >128 chars rejected");

    // Invalid symbol → bad_args refusal from the lib itself.
    const auto bad = SymbolQuery::findDefinition(root, QStringLiteral("a.b"), def);
    expect(!bad.ok && bad.code == QStringLiteral("bad_args"),
           "INV-8: invalid symbol refused with bad_args");

    // ANTS-1950 — file-stem fallback hint. `app` is a valid identifier but no
    // symbol named `app` exists; it is the base name of app.py, so the result
    // carries fileStemHint pointing at the file.
    const auto stem = SymbolQuery::findDefinition(root, QStringLiteral("app"), def);
    expect(stem.definitions.isEmpty(), "ANTS-1950: no symbol named app");
    expect(stem.fileStemHint == QStringLiteral("app.py"),
           "ANTS-1950: fileStemHint points at app.py");
    // A symbol that DOES resolve must not be second-guessed with a stem hint
    // even if it shares a file's name (none here defines a same-named file, so
    // a resolving symbol simply carries an empty hint).
    const auto resolved = SymbolQuery::findDefinition(
        root, QStringLiteral("compute"), def);
    expect(resolved.fileStemHint.isEmpty(),
           "ANTS-1950: resolving symbol carries no stem hint");

    // ANTS-1700 — a namespace-qualified *call* site (`ns::sym(`) must not
    // be mis-classified as a definition. The C++ def anchor now requires a
    // return-type token before the (optionally qualified) name.
    const auto sb = SymbolQuery::findDefinition(root, QStringLiteral("slurpBody"), def);
    expect(sb.ok, "ANTS-1700: findDefinition(slurpBody) ok");
    expect(hasDef(sb, QStringLiteral("src/calls.cpp"), QStringLiteral("definition")),
           "ANTS-1700: real qualified def 'QByteArray ns::slurpBody(...)' found");
    expect(sb.definitionsTotal == 1,
           "ANTS-1700: qualified call sites not counted as definitions "
           "(definitionsTotal must be exactly 1)");
    bool noCallAsDef = true;
    for (const auto &m : sb.definitions)
        if (m.signature.contains(QStringLiteral("ns::slurpBody(\"x\")")) ||
            m.signature.startsWith(QStringLiteral("auto y")))
            noCallAsDef = false;
    expect(noCallAsDef, "ANTS-1700: no call line reported as a definition");

    // ANTS-2146 — a bare call in statement position (`return slurpBody(p);`)
    // must not be absorbed as a return-type token and mis-tagged as a
    // `declaration`. definitionsTotal stays 1 (INV above) and no emitted
    // signature begins with the `return ` keyword.
    bool noReturnCallAsDef = true;
    for (const auto &m : sb.definitions)
        if (m.signature.startsWith(QStringLiteral("return ")))
            noReturnCallAsDef = false;
    expect(noReturnCallAsDef,
           "ANTS-2146: return-position call not reported as a definition");

    EXPECT_EQ(0, expect_failures());
}

// ANTS-2150 — brace-family generic anchors: Rust / Go / TypeScript / Java.
// One def + one caller per language proves the generic def/call patterns fire,
// and the lang filter restricts to Lang::Generic.
TEST(McpSymbolQuery, BraceFamilyLanguages) {
    expect_reset();

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // Rust — keyword fn/struct + a caller.
    writeFile(root, QStringLiteral("src/lib.rs"),
              QStringLiteral("pub fn rustParse(path: &str) -> u32 {\n"
                             "    helper()\n"
                             "}\n"
                             "pub struct RustCfg {\n"
                             "    x: u32,\n"
                             "}\n"
                             "fn rust_caller() {\n"
                             "    rustParse(\"x\");\n"   // caller
                             "}\n"));
    // Go — bare func, method-with-receiver, type struct + a caller.
    writeFile(root, QStringLiteral("src/server.go"),
              QStringLiteral("func goNewServer(addr string) *Server {\n"
                             "    return nil\n"
                             "}\n"
                             "func (s *Server) goStart() error {\n"
                             "    goNewServer(\"x\")\n"   // caller
                             "    return nil\n"
                             "}\n"
                             "type Server struct {\n"
                             "}\n"));
    // TypeScript — class, arrow assignment, function + a caller.
    writeFile(root, QStringLiteral("src/service.ts"),
              QStringLiteral("export class TsService {\n"
                             "}\n"
                             "export const tsHandler = (req, res) => {\n"
                             "    return tsLoad()\n"     // caller
                             "}\n"
                             "function tsLoad() {\n"
                             "    return 1\n"
                             "}\n"));
    // Java — class + a keyword-less C-style method definition.
    writeFile(root, QStringLiteral("src/Account.java"),
              QStringLiteral("public class JavaAccount {\n"
                             "    private void javaDoStuff() {\n"
                             "        helper();\n"
                             "    }\n"
                             "}\n"));

    SymbolQuery::Options def;

    auto defFound = [&](const char *sym) {
        return SymbolQuery::findDefinition(root, QString::fromUtf8(sym), def)
                   .definitions.size() >= 1;
    };
    // Rust
    expect(defFound("rustParse"), "rust: fn def found");
    expect(defFound("RustCfg"),   "rust: struct def found");
    // Go
    expect(defFound("goNewServer"), "go: func def found");
    expect(defFound("goStart"),     "go: method-with-receiver def found");
    expect(defFound("Server"),      "go: type struct def found");
    // TypeScript
    expect(defFound("TsService"), "ts: class def found");
    expect(defFound("tsHandler"), "ts: arrow-assignment def found");
    expect(defFound("tsLoad"),    "ts: function def found");
    // Java
    expect(defFound("JavaAccount"), "java: class def found");
    expect(defFound("javaDoStuff"), "java: C-style method def found");

    // Callers fire across the family.
    expect(SymbolQuery::findCaller(root, QStringLiteral("rustParse"), def)
               .callers.size() >= 1, "rust: caller found");
    expect(SymbolQuery::findCaller(root, QStringLiteral("goNewServer"), def)
               .callers.size() >= 1, "go: caller found");
    expect(SymbolQuery::findCaller(root, QStringLiteral("tsLoad"), def)
               .callers.size() >= 1, "ts: caller found");

    // The def line itself is never reported as a caller (INV-9 parity).
    bool noDefAsCaller = true;
    for (const auto &cm : SymbolQuery::findCaller(
             root, QStringLiteral("goNewServer"), def).callers)
        if (cm.context.startsWith(QStringLiteral("func goNewServer")))
            noDefAsCaller = false;
    expect(noDefAsCaller, "generic: definition line not reported as caller");

    // lang filter: Lang::Generic finds the brace-family def; Lang::Py does not.
    SymbolQuery::Options gen;  gen.lang = SymbolQuery::Lang::Generic;
    SymbolQuery::Options pyf;  pyf.lang = SymbolQuery::Lang::Py;
    expect(SymbolQuery::findDefinition(root, QStringLiteral("rustParse"), gen)
               .definitions.size() >= 1, "lang=generic finds the rust def");
    expect(SymbolQuery::findDefinition(root, QStringLiteral("rustParse"), pyf)
               .definitions.isEmpty(), "lang=py finds no brace-family def");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpSymbolQuery, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // INV-9 — declarations + definitions + IPC dispatch.
    expect(contains(rcHdr, "cmdFindDefinition") &&
               contains(rcHdr, "cmdFindCaller"),
           "INV-9: handlers declared in remotecontrol.h");
    expect(contains(rcCpp, "RemoteControl::cmdFindDefinition"),
           "INV-9: cmdFindDefinition defined");
    expect(contains(rcCpp, "RemoteControl::cmdFindCaller"),
           "INV-9: cmdFindCaller defined");
    expect(contains(rcCpp, "\"find-definition\"") &&
               contains(rcCpp, "\"find-caller\""),
           "INV-9: IPC dispatch verbs wired");

    // INV-10 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"find_definition\"") &&
               contains(mwCpp, "registerToolProvider(\"find_caller\""),
           "INV-10: both tools registered in mainwindow.cpp");

    // INV-11 — claudeintegration descriptors, token-cost, kind, contract.
    expect(contains(ciCpp, "t[\"name\"] = \"find_definition\"") &&
               contains(ciCpp, "t[\"name\"] = \"find_caller\""),
           "INV-11: tool descriptors present");
    expect(contains(ciCpp, "QStringLiteral(\"find_definition\"),    {600,  2500}") ||
               contains(ciCpp, "\"find_definition\"),    {600,  2500}"),
           "INV-11: find_definition token-cost entry");
    expect(contains(ciCpp, "\"find_caller\"),        {800,  4000}"),
           "INV-11: find_caller token-cost entry");
    expect(contains(ciCpp, "QStringLiteral(\"symbol\")"),
           "INV-11: kindForName has a \"symbol\" bucket");
    expect(contains(ciCpp,
               "if (toolName == QStringLiteral(\"find_definition\"))    return C::Required;"),
           "INV-11: find_definition contract Required");
    expect(contains(ciCpp,
               "if (toolName == QStringLiteral(\"find_caller\"))        return C::Required;"),
           "INV-11: find_caller contract Required");

    EXPECT_EQ(0, expect_failures());
}

// ANTS-2087 — opt-in include_body: find_definition / find_caller attach
// the symbol body inline via read_region's extractor. The body
// extraction itself is covered by ReadRegion's own tests (ANTS-2021);
// here we lock in the wiring + schema surface (cmdFind* has no public
// test seam, so this mirrors the INV-9/10/11 source-scrape contract).
TEST(McpSymbolQuery, Ants2087IncludeBodyWired) {
    const std::string rcCpp =
        ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Glue helper exists and reuses read_region's symbol-body extractor.
    EXPECT_NE(rcCpp.find("sqAttachBody"), std::string::npos)
        << "sqAttachBody helper missing";
    EXPECT_NE(rcCpp.find("ReadRegion::extract"), std::string::npos)
        << "sqAttachBody must reuse ReadRegion::extract, not re-slice";

    // cmdFindDefinition reads include_body and calls the helper.
    const auto defPos = rcCpp.find("RemoteControl::cmdFindDefinition");
    ASSERT_NE(defPos, std::string::npos);
    const auto defEnd = rcCpp.find("RemoteControl::cmdFindCaller", defPos);
    ASSERT_NE(defEnd, std::string::npos);
    const std::string defBody = rcCpp.substr(defPos, defEnd - defPos);
    EXPECT_NE(defBody.find("include_body"), std::string::npos)
        << "cmdFindDefinition must read include_body";
    EXPECT_NE(defBody.find("sqAttachBody"), std::string::npos)
        << "cmdFindDefinition must attach the body when requested";

    // cmdFindCaller reads include_body for its definition echo.
    const auto callPos = rcCpp.find("RemoteControl::cmdFindCaller");
    ASSERT_NE(callPos, std::string::npos);
    const std::string callBody = rcCpp.substr(callPos, 1800);
    EXPECT_NE(callBody.find("include_body"), std::string::npos)
        << "cmdFindCaller must read include_body";

    // Schema declares include_body on BOTH descriptors.
    int n = 0;
    size_t i = 0;
    const std::string needle = "props[\"include_body\"]";
    while ((i = ciCpp.find(needle, i)) != std::string::npos) {
        ++n;
        i += needle.size();
    }
    EXPECT_GE(n, 2)
        << "include_body must be declared on both find_definition + "
           "find_caller schemas; found " << n;
}
