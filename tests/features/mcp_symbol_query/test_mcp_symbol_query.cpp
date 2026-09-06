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
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
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

    // --- ANTS-4603: a return type wrapped onto its own line ------------
    // The convention for a long return type, and every def anchor matches
    // ONE line, so the qualified-name line carried no return-type token and
    // resolved to nothing — reading as "no such symbol" for a function that
    // is plainly there. Measured on this tree: 30 such definitions, all
    // invisible. The fixture pairs each real case with the false positive
    // that kept the anchor single-line for so long.
    writeFile(root, QStringLiteral("src/wrapped.cpp"),
              QStringLiteral("#include \"widget.h\"\n"
                             "std::optional<Widget::Handle>\n"
                             "Widget::acquireHandle(int slot) {\n"   // wrapped def
                             "    return {};\n"
                             "}\n"
                             "const std::vector<Widget::Span> &\n"
                             "Widget::spansForRow(int row) const {\n"  // wrapped, ref return
                             "    return m_spans;\n"
                             "}\n"
                             "void Widget::caller() {\n"
                             "    int total =\n"
                             "        Widget::acquireHandle(1);\n"   // NOT a def: indented
                             "}\n"
                             // Each negative leg defeats ONE half of the pair,
                             // so neither half can go untested behind the
                             // other. This one is at column 0 — the line
                             // anchor passes and only the previous-line test
                             // can reject it, because `=` is not a return
                             // type.
                             "static const int kSeats =\n"
                             "Widget::spansForRow(2);\n"
                             // And this one has a return-type-shaped line
                             // above it, so only the column anchor rejects it.
                             "int Widget::rows() const {\n"
                             "    QVector<int> out;\n"
                             "    Widget::spansForRow(3);\n"
                             "    return 0;\n"
                             "}\n"));

    // --- ANTS-3465: C++ type definitions (struct/class/union/enum) -----
    // Opening brace on the keyword line → kind "definition"; a trailing `;`
    // forward declaration → kind "declaration". None have a same-named
    // constructor, so pre-fix these returned definitions_count:0.
    writeFile(root, QStringLiteral("src/types.h"),
              QStringLiteral("struct PlainStruct {\n"
                             "    int x;\n"
                             "};\n"
                             "class PlainClass {\n"
                             "public:\n"
                             "    int y;\n"
                             "};\n"
                             "enum class Color {\n"
                             "    Red,\n"
                             "    Green\n"
                             "};\n"
                             "union Wrap {\n"
                             "    int i;\n"
                             "    float f;\n"
                             "};\n"
                             "struct FwdOnly;\n"));

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

    // ANTS-3465 — C++ type definitions resolve (struct/class/union/enum),
    // not just functions. A brace-on-keyword-line is a definition; a
    // trailing-`;` forward decl is a declaration.
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("PlainStruct"), def),
                  QStringLiteral("src/types.h"), QStringLiteral("definition")),
           "ANTS-3465: struct definition found");
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("PlainClass"), def),
                  QStringLiteral("src/types.h"), QStringLiteral("definition")),
           "ANTS-3465: class definition found");
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("Color"), def),
                  QStringLiteral("src/types.h"), QStringLiteral("definition")),
           "ANTS-3465: enum class definition found");
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("Wrap"), def),
                  QStringLiteral("src/types.h"), QStringLiteral("definition")),
           "ANTS-3465: union definition found");
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("FwdOnly"), def),
                  QStringLiteral("src/types.h"), QStringLiteral("declaration")),
           "ANTS-3465: forward declaration tagged as declaration");

    // ANTS-4603 — a wrapped return type resolves, and the two shapes that
    // look like it do not. The negative legs are the whole reason this
    // anchor is a PAIR: the qualified-name line alone is indistinguishable
    // from a statement-position call, so an anchor without the previous-line
    // test would report both of them as definitions.
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("acquireHandle"), def),
                  QStringLiteral("src/wrapped.cpp"), QStringLiteral("definition")),
           "ANTS-4603: wrapped return type resolves");
    expect(hasDef(SymbolQuery::findDefinition(root, QStringLiteral("spansForRow"), def),
                  QStringLiteral("src/wrapped.cpp"), QStringLiteral("definition")),
           "ANTS-4603: wrapped reference return type resolves (trailing &)");
    // Exactly one definition each: the indented call and the return-position
    // call in the same file must not have been counted as a second.
    expect(SymbolQuery::findDefinition(root, QStringLiteral("acquireHandle"), def)
               .definitionsTotal == 1,
           "ANTS-4603: an indented qualified call is not a definition");
    expect(SymbolQuery::findDefinition(root, QStringLiteral("spansForRow"), def)
               .definitionsTotal == 1,
           "ANTS-4603: a column-0 call under `=` is not a definition, and an "
           "indented one under a return-type-shaped line is not either");

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
    // Ruby (ANTS-2150) — folds into Lang::Generic: def/class/module + a
    // `def self.x` singleton (exercises the optional `self.` receiver in the
    // Generic def anchor). Paren-less Ruby calls aren't matched by the shared
    // Generic call anchor, so this pass covers definitions/outline, not callers.
    writeFile(root, QStringLiteral("src/thing.rb"),
              QStringLiteral("module RubyMod\n"
                             "  class RubyThing\n"
                             "    def ruby_instance\n"
                             "      true\n"
                             "    end\n"
                             "    def self.ruby_singleton\n"
                             "      RubyThing.new\n"
                             "    end\n"
                             "  end\n"
                             "end\n"));

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
    // Ruby (folded into Lang::Generic — ANTS-2150)
    expect(defFound("RubyMod"),        "ruby: module def found");
    expect(defFound("RubyThing"),      "ruby: class def found");
    expect(defFound("ruby_instance"),  "ruby: instance def found");
    expect(defFound("ruby_singleton"), "ruby: `def self.` singleton def found");

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

// ANTS-3558 — GLSL / shader family. The distinguishing case vs the Generic
// brace-family anchor is the Allman next-line-brace style (`uint pcgHash(uint
// x)\n{`) that is common in shader code — Generic requires the `{` on the def
// line and misses it, so before this family GLSL projects (DOOM_Ants) got a
// silent zero from find_definition. The Glsl def anchor stops at `(`.
TEST(McpSymbolQuery, Ants3558GlslShaderFamily) {
    expect_reset();

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // .glsl — Allman next-line braces (the case Generic misses).
    writeFile(root, QStringLiteral("shaders/pt_common.glsl"),
              QStringLiteral("uint pcgHash(uint x)\n"
                             "{\n"
                             "    return x * 747796405u + 2891336453u;\n"
                             "}\n"
                             "vec3 decodeAlbedo(uint id, vec2 uv)\n"
                             "{\n"
                             "    return vec3(0.0);\n"
                             "}\n"));
    // .comp — same-line brace def, a prototype (declaration), and a caller.
    writeFile(root, QStringLiteral("shaders/pathtrace.comp"),
              QStringLiteral("vec2 detilePOM(vec2 baseUV);\n"          // prototype
                             "vec3 hash3(ivec2 cell) {\n"
                             "    return vec3(float(pcgHash(uint(cell.x))));\n"  // caller
                             "}\n"));

    SymbolQuery::Options def;
    auto defFound = [&](const char *sym) {
        return SymbolQuery::findDefinition(root, QString::fromUtf8(sym), def)
                   .definitions.size() >= 1;
    };
    expect(defFound("pcgHash"),      "glsl: next-line-brace def found (Generic misses this)");
    expect(defFound("decodeAlbedo"), "glsl: second next-line-brace def found");
    expect(defFound("hash3"),        "glsl: same-line-brace def found");

    // A trailing-`;` prototype is a declaration, not a definition (cppKind).
    const auto dp =
        SymbolQuery::findDefinition(root, QStringLiteral("detilePOM"), def);
    expect(dp.definitions.size() >= 1, "glsl: prototype located");
    expect(dp.definitions.front().kind == QStringLiteral("declaration"),
           "glsl: trailing-; prototype tagged declaration");

    // The caller inside hash3's body resolves.
    expect(SymbolQuery::findCaller(root, QStringLiteral("pcgHash"), def)
               .callers.size() >= 1, "glsl: caller of pcgHash found");

    // lang filter: Lang::Glsl scans the shader files; Lang::Generic does not
    // (the .glsl/.comp extensions map to Glsl, so the Generic pass skips them).
    SymbolQuery::Options gl;   gl.lang = SymbolQuery::Lang::Glsl;
    SymbolQuery::Options gen;  gen.lang = SymbolQuery::Lang::Generic;
    expect(SymbolQuery::findDefinition(root, QStringLiteral("pcgHash"), gl)
               .definitions.size() >= 1, "lang=glsl finds the shader def");
    expect(SymbolQuery::findDefinition(root, QStringLiteral("pcgHash"), gen)
               .definitions.isEmpty(), "lang=generic skips shader files");

    // parseLang round-trips the new family name.
    expect(SymbolQuery::parseLang(QStringLiteral("glsl")) ==
           SymbolQuery::Lang::Glsl, "parseLang(\"glsl\") == Lang::Glsl");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpSymbolQuery, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
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
        ants_test::slurpRemoteControl();
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

    // cmdFindCaller reads include_body for its definition echo. Bound the
    // scrape by the next method definition rather than a fixed byte window —
    // ANTS-3555's files_only branch grew the body and pushed the
    // definition-echo `include_body` past the old 1800-byte window.
    const auto callPos = rcCpp.find("RemoteControl::cmdFindCaller");
    ASSERT_NE(callPos, std::string::npos);
    const auto callEnd =
        rcCpp.find("\nQJsonDocument RemoteControl::", callPos + 1);
    const std::string callBody = rcCpp.substr(
        callPos,
        callEnd == std::string::npos ? std::string::npos : callEnd - callPos);
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

// ANTS-3555 — files_only manifest mode: find_caller returns the distinct
// matched-file set (per-file call count + the exact line numbers) and drops
// the quoted per-call `context` windows — the manifest the caller reads_region
// next, not the re-sent code around every call. cmdFindCaller has no public
// test seam (see ANTS-2087 above), so this locks the envelope + schema via the
// same source-scrape contract INV-9/10/11 use.
TEST(McpSymbolQuery, Ants3555FilesOnlyManifest) {
    const std::string rcCpp =
        ants_test::slurpRemoteControl();
    const std::string ciCpp =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // Scope to cmdFindCaller's body (bounded by the next method definition so
    // the scrape can't bleed into a neighbour).
    const auto callPos = rcCpp.find("RemoteControl::cmdFindCaller");
    ASSERT_NE(callPos, std::string::npos);
    const auto callEnd =
        rcCpp.find("\nQJsonDocument RemoteControl::", callPos + 1);
    const std::string callBody = rcCpp.substr(
        callPos,
        callEnd == std::string::npos ? std::string::npos : callEnd - callPos);

    // INV-1 — files_only arg parsed inside cmdFindCaller.
    EXPECT_NE(callBody.find("\"files_only\""), std::string::npos)
        << "cmdFindCaller must parse the files_only arg";

    // INV-2 — manifest envelope: emits per-file line numbers and the branch
    // early-returns BEFORE the full callers[] loop, so the quoted `context`
    // windows are never built into the reply.
    const auto branchPos = callBody.find("if (filesOnly)");
    EXPECT_NE(branchPos, std::string::npos)
        << "files_only early-return branch present in cmdFindCaller";
    EXPECT_NE(callBody.find("\"lines\""), std::string::npos)
        << "files_only manifest carries per-file line numbers";
    const auto ctxLoopPos = callBody.find("o[\"context\"]");
    if (branchPos != std::string::npos && ctxLoopPos != std::string::npos) {
        EXPECT_LT(branchPos, ctxLoopPos)
            << "files_only must return BEFORE the context-quoting caller loop";
    }

    // INV-3 — tools/list schema enumerates files_only within the find_caller
    // descriptor block (bounded by the next descriptor).
    const auto fcSchema = ciCpp.find("t[\"name\"] = \"find_caller\"");
    ASSERT_NE(fcSchema, std::string::npos);
    const auto fcSchemaEnd = ciCpp.find("t[\"name\"] =", fcSchema + 1);
    const std::string fcBlock = ciCpp.substr(
        fcSchema,
        fcSchemaEnd == std::string::npos ? std::string::npos
                                         : fcSchemaEnd - fcSchema);
    EXPECT_NE(fcBlock.find("props[\"files_only\"]"), std::string::npos)
        << "find_caller tools/list schema must enumerate files_only";
}

// ─────────────────────────────────────────────────────────────────────
// The C++ matcher's four reported blind spots, fixed in one pass:
// ANTS-4346 (namespace + case-insensitive stem hint), ANTS-4358
// (auto NAME = [ lambda assignment), ANTS-4368 (extern "C" linkage
// prefix) and ANTS-4369 (a trailing comment defeating the decl test).
// Reported independently by DOOM, Ants Terminal and AI_Prompts.
// ─────────────────────────────────────────────────────────────────────
namespace {

// One tree carrying all four shapes, plus the controls that isolate each.
QString cppFormsRoot(QTemporaryDir &tmp) {
    const QString root = tmp.path();
    // ANTS-3668 — data-member declarations. Measured as the single largest
    // classified population of unresolved doc_symbols candidates, and every
    // one of them resolved nowhere: the Cpp ladder had a return-type-led
    // form requiring `(`, an out-of-line ctor/dtor form, and a
    // struct/class/union/enum keyword form. A field matches none.
    writeFile(root, QStringLiteral("src/members.h"), QStringLiteral(
        "#pragma once\n"
        "#include <QTimer>\n"
        "\n"
        "class Widget {\n"
        "public:\n"
        "    void paint();               // a method, for contrast\n"
        "private:\n"
        "    int m_scrollOffset = 0;\n"
        "    QTimer *m_claudeDetectTimer = nullptr;\n"
        "    QPushButton *m_claudeReviewBtn{nullptr};\n"
        "    static const int kFrozenRows;\n"
        "    QMap<QString, QStringList> m_byCategory;\n"
        "    int m_sizes[3]{1, 2, 3};\n"
        "    std::map<int, int> m_pairs{{1, 2}};\n"
        "};\n"
        "\n"
        "struct Point { int x; int y; };\n"
        "\n"
        "void onlyEverCalled();\n"))
        ;
    writeFile(root, QStringLiteral("src/members.cpp"), QStringLiteral(
        "#include \"members.h\"\n"
        "\n"
        "void Widget::paint() {\n"
        "    onlyEverCalled();\n"
        "    m_scrollOffset = 1;\n"
        "}\n"));
    writeFile(root, QStringLiteral("src/seam.cpp"), QStringLiteral(
        "#include \"seam.h\"\n"
        "\n"
        "// Control for ANTS-4368: same file, same Allman brace, NO linkage\n"
        "// prefix. If this resolves and RB_VulkanProbe does not, the prefix\n"
        "// is the only variable.\n"
        "void CreateFramebuffers()\n"
        "{\n"
        "}\n"
        "\n"
        "extern \"C\" int RB_VulkanProbe(void)\n"
        "{\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "extern \"C++\" void CppLinkageThing(void)\n"
        "{\n"
        "}\n"
        "\n"
        "void Host()\n"
        "{\n"
        "    auto makeEtagMatchProp = []{\n"
        "        return 1;\n"
        "    };\n"
        "    static const auto makeFieldsProp = [](int n) { return n; };\n"
        "    static const auto makeRawProp = []{ return 2; };\n"
        "}\n"
        "\n"
        "namespace SeamSpace {\n"
        "int inner = 0;\n"
        "}\n"));
    writeFile(root, QStringLiteral("src/seam.h"), QStringLiteral(
        "#pragma once\n"
        "extern int  RB_VulkanProbe(void);   // trailing comment, ANTS-4369\n"
        "extern void RB_Vulkan_Present(void);\n"));
    return root;
}

bool hasKindAt(const SymbolQuery::DefResult &r, const char *fileSuffix,
               const char *kind) {
    for (const auto &d : r.definitions)
        if (d.file.endsWith(QLatin1String(fileSuffix)) &&
            d.kind == QLatin1String(kind))
            return true;
    return false;
}

}  // namespace

// ANTS-4368 — an `extern "C"` definition is a definition, not just the
// header prototype. The control proves the linkage prefix is the variable.
TEST(SymbolQueryCppForms, ExternCDefinitionsResolve) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;

    // Control: no linkage prefix, same brace style -> already worked.
    const auto control =
        SymbolQuery::findDefinition(root, QStringLiteral("CreateFramebuffers"), o);
    EXPECT_TRUE(hasKindAt(control, "seam.cpp", "definition"))
        << "control must resolve, or the test proves nothing about extern \"C\"";

    const auto probe =
        SymbolQuery::findDefinition(root, QStringLiteral("RB_VulkanProbe"), o);
    EXPECT_TRUE(hasKindAt(probe, "seam.cpp", "definition"))
        << "the extern \"C\" body in the .cpp must be found, not only the "
           "header prototype — returning the prototype with ok:true and "
           "definitions_count:1 is a confident wrong answer";
    EXPECT_TRUE(hasKindAt(probe, "seam.h", "declaration"));

    EXPECT_TRUE(hasKindAt(
        SymbolQuery::findDefinition(root, QStringLiteral("CppLinkageThing"), o),
        "seam.cpp", "definition")) << "extern \"C++\" too";
}

// ANTS-4358 — `auto NAME = [...]{...}` inside a function body.
TEST(SymbolQueryCppForms, LambdaAssignmentDefinitionsResolve) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;

    EXPECT_TRUE(hasKindAt(
        SymbolQuery::findDefinition(root, QStringLiteral("makeEtagMatchProp"), o),
        "seam.cpp", "definition"))
        << "auto NAME = []{ at function-body indent";
    EXPECT_TRUE(hasKindAt(
        SymbolQuery::findDefinition(root, QStringLiteral("makeFieldsProp"), o),
        "seam.cpp", "definition"))
        << "static const auto NAME = [](args) { ... }";
}

// ANTS-4369 — a trailing `//` comment must not turn a prototype into a
// definition. The two header lines differ ONLY by the comment.
TEST(SymbolQueryCppForms, TrailingCommentDoesNotMakeADeclarationADefinition) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;

    const auto commented =
        SymbolQuery::findDefinition(root, QStringLiteral("RB_VulkanProbe"), o);
    for (const auto &d : commented.definitions) {
        if (!d.file.endsWith(QLatin1String("seam.h"))) continue;
        EXPECT_EQ(d.kind, QStringLiteral("declaration"))
            << "a prototype with a trailing comment is still a prototype: "
            << d.signature.toStdString();
    }

    const auto plain =
        SymbolQuery::findDefinition(root, QStringLiteral("RB_Vulkan_Present"), o);
    EXPECT_TRUE(hasKindAt(plain, "seam.h", "declaration"))
        << "control: the uncommented sibling was always right";
}

// ANTS-4346 — a namespace resolves, and the stem hint is case-insensitive.
TEST(SymbolQueryCppForms, NamespaceResolvesAndStemHintIsCaseInsensitive) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;

    const auto ns =
        SymbolQuery::findDefinition(root, QStringLiteral("SeamSpace"), o);
    EXPECT_FALSE(ns.definitions.isEmpty())
        << "a namespace is a definition a caller can ask for";
    EXPECT_TRUE(hasKindAt(ns, "seam.cpp", "namespace"))
        << "and it is tagged `namespace`, so a caller can tell it from a "
           "function without opening the file";

    // PascalCase query against a lowercase filename — this repo's convention,
    // which the case-sensitive compare could never match.
    const auto miss =
        SymbolQuery::findDefinition(root, QStringLiteral("Seam"), o);
    EXPECT_FALSE(miss.fileStemHint.isEmpty())
        << "no symbol `Seam`, but src/seam.cpp exists — the ANTS-1950 rescue "
           "must fire across case or it never fires on this codebase";
}

// ANTS-3746 — a C++ return type carrying a template argument list with a
// COMMA. The return-type group is `(?:[\w:<>~]+[\s*&]+)+`: a comma is neither
// in the token class nor a separator, so the group can never span
// `QMap<QString, QStringList>` and the definition is reported as absent.
//
// The cost is not a retry. A zero from find_definition is byte-identical to
// "no such symbol", so the caller's next move is to design around a function
// that is plainly there. Measured on the live tree: `detectorsByCategory`,
// declared at src/debtsweepengine.h:264 and defined at
// src/debtsweepengine.cpp:1344, returned definitions_count:0 over 911 files.
TEST(McpSymbolQuery, Ants3746CommaBearingTemplateReturnType) {
    expect_reset();

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    writeFile(root, QStringLiteral("src/registry.h"),
              QStringLiteral(
                  "#pragma once\n"
                  "const QMap<QString, QStringList> &twoArgRef();\n"
                  "QMap<QString, QStringList> *twoArgPtr();\n"
                  "std::pair<int, int> pairByValue();\n"
                  "std::map<QString, std::vector<int>> nestedTemplate();\n"));
    writeFile(root, QStringLiteral("src/registry.cpp"),
              QStringLiteral(
                  "#include \"registry.h\"\n"
                  "const QMap<QString, QStringList> &twoArgRef() {\n"
                  "    static QMap<QString, QStringList> m;\n"
                  "    return m;\n"
                  "}\n"
                  "QMap<QString, QStringList> *twoArgPtr() {\n"
                  "    return nullptr;\n"
                  "}\n"
                  "std::pair<int, int> pairByValue() {\n"
                  "    return {0, 0};\n"
                  "}\n"
                  "std::map<QString, std::vector<int>> nestedTemplate() {\n"
                  "    return {};\n"
                  "}\n"));

    SymbolQuery::Options o;

    // The exact shape measured on the live tree: `const T<A, B> &name()`.
    const auto ref = SymbolQuery::findDefinition(
        root, QStringLiteral("twoArgRef"), o);
    expect(hasDef(ref, QStringLiteral("src/registry.cpp"),
                  QStringLiteral("definition")),
           "ANTS-3746: `const QMap<QString, QStringList> &f()` is a definition");
    expect(hasDef(ref, QStringLiteral("src/registry.h"),
                  QStringLiteral("declaration")),
           "ANTS-3746: its header prototype is a declaration");

    // Pointer and by-value variants of the same defect.
    expect(hasDef(SymbolQuery::findDefinition(
                      root, QStringLiteral("twoArgPtr"), o),
                  QStringLiteral("src/registry.cpp"),
                  QStringLiteral("definition")),
           "ANTS-3746: the `*` variant resolves too");
    expect(hasDef(SymbolQuery::findDefinition(
                      root, QStringLiteral("pairByValue"), o),
                  QStringLiteral("src/registry.cpp"),
                  QStringLiteral("definition")),
           "ANTS-3746: `std::pair<int, int> f()` by value resolves");

    // One level of nesting, which is what makes a naive `<[^>]*>` wrong.
    expect(hasDef(SymbolQuery::findDefinition(
                      root, QStringLiteral("nestedTemplate"), o),
                  QStringLiteral("src/registry.cpp"),
                  QStringLiteral("definition")),
           "ANTS-3746: a nested template argument list resolves");

    EXPECT_EQ(0, expect_failures());
}

// ANTS-3668 — a data member is a definition of a name, and the Cpp ladder
// resolved none. Measured via the doc_symbols corpus calibration as the
// single largest classified population of unresolved candidates: every
// struct field, every `m_`-prefixed member, resolving nowhere.
TEST(SymbolQueryCppForms, Ants3668DataMembersResolve) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;

    // Resolution is what this item is about; the kind is asserted separately
    // below, because the two answers differ for a brace-initialised member.
    for (const char *name : {"m_scrollOffset", "m_claudeDetectTimer",
                             "m_claudeReviewBtn", "kFrozenRows"}) {
        const auto d =
            SymbolQuery::findDefinition(root, QString::fromUtf8(name), o);
        bool found = false;
        for (const auto &m : d.definitions)
            if (m.file.endsWith(QLatin1String("members.h"))) found = true;
        EXPECT_TRUE(found) << "data member did not resolve: " << name;
    }

    // The kind itself is ANTS-4821's subject; this is the `=`-initialised
    // spelling, which was already right.
    EXPECT_TRUE(hasKindAt(SymbolQuery::findDefinition(
                              root, QStringLiteral("m_scrollOffset"), o),
                          "members.h", "declaration"))
        << "int m_scrollOffset = 0;";
}

// ANTS-3680 — `findDefinitions` resolves N needles in one walk. Its whole
// contract is that it answers exactly what N separate `findDefinition` calls
// answer, so the test is that equivalence rather than a hand-written expected
// set: a batch gate that silently dropped a needle would still look plausible
// against literals.
//
// The tree carries the case the batch gate could get wrong on its own — a
// needle that is a strict prefix of a longer identifier (`parseLine` inside
// `parseLineExtra`, `m_count` inside `m_countTotal`, `load` inside `loader`) —
// in every language family, plus a stem-hint-only needle, an invalid needle
// and a duplicate.
namespace {

QString batchRoot(QTemporaryDir &tmp) {
    const QString root = tmp.path();
    writeFile(root, QStringLiteral("src/parse.h"), QStringLiteral(
        "#pragma once\n"
        "class Parser {\n"
        "public:\n"
        "    int parseLine(const QString &s);\n"
        "    int parseLineExtra(const QString &s, int n);\n"
        "private:\n"
        "    int m_count = 0;\n"
        "    int m_countTotal{0};\n"
        "};\n"));
    writeFile(root, QStringLiteral("src/parse.cpp"), QStringLiteral(
        "#include \"parse.h\"\n"
        "int Parser::parseLine(const QString &s) {\n"
        "    m_count += parseLineExtra(s, 1);\n"
        "    return m_count;\n"
        "}\n"
        "int Parser::parseLineExtra(const QString &s, int n) {\n"
        "    m_countTotal += n;\n"
        "    return n;\n"
        "}\n"));
    writeFile(root, QStringLiteral("app.py"), QStringLiteral(
        "def load(path):\n"
        "    return loader(path)\n"
        "def loader(path):\n"
        "    return path\n"));
    writeFile(root, QStringLiteral("mod.lua"), QStringLiteral(
        "function run(x)\n"
        "  return runAll(x)\n"
        "end\n"
        "function runAll(x)\n"
        "  return x\n"
        "end\n"));
    writeFile(root, QStringLiteral("script.sh"), QStringLiteral(
        "#!/bin/sh\n"
        "deploy() {\n"
        "  deployAll\n"
        "}\n"));
    // ANTS-1950 — a file whose stem is a needle that resolves nowhere, so the
    // batch has to reproduce the single path's stem hint too.
    writeFile(root, QStringLiteral("src/orphan_stem.cpp"), QStringLiteral(
        "// no symbol of that name anywhere\n"));
    return root;
}

}  // namespace

TEST(SymbolQueryBatch, Ants3680MatchesPerSymbolCalls) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = batchRoot(tmp);

    const QStringList needles{
        QStringLiteral("parseLine"),   QStringLiteral("parseLineExtra"),
        QStringLiteral("m_count"),     QStringLiteral("m_countTotal"),
        QStringLiteral("load"),        QStringLiteral("loader"),
        QStringLiteral("run"),         QStringLiteral("runAll"),
        QStringLiteral("deploy"),      QStringLiteral("orphan_stem"),
        QStringLiteral("nowhere"),     QStringLiteral("parseLine"),  // duplicate
        QStringLiteral("1bad")};                                     // invalid

    for (const int cap : {0, 1}) {   // 0 = default cap; 1 = per-needle cap bites
        SymbolQuery::Options o;
        o.maxResults = cap;
        const auto batch = SymbolQuery::findDefinitions(root, needles, o);
        EXPECT_EQ(batch.size(), 12) << "duplicate needle answered twice";

        for (const QString &n : needles) {
            const SymbolQuery::DefResult one = SymbolQuery::findDefinition(root, n, o);
            ASSERT_TRUE(batch.contains(n)) << "batch dropped needle: " << qPrintable(n);
            const SymbolQuery::DefResult &b = batch.value(n);

            EXPECT_EQ(b.ok, one.ok) << qPrintable(n);
            EXPECT_EQ(b.code, one.code) << qPrintable(n);
            EXPECT_EQ(b.definitionsTotal, one.definitionsTotal) << qPrintable(n);
            EXPECT_EQ(b.truncated, one.truncated) << qPrintable(n);
            EXPECT_EQ(b.walkCapped, one.walkCapped) << qPrintable(n);
            EXPECT_EQ(b.fileStemHint, one.fileStemHint) << qPrintable(n);
            ASSERT_EQ(b.definitions.size(), one.definitions.size()) << qPrintable(n);
            for (int i = 0; i < b.definitions.size(); ++i) {
                EXPECT_EQ(b.definitions[i].file, one.definitions[i].file) << qPrintable(n);
                EXPECT_EQ(b.definitions[i].line, one.definitions[i].line) << qPrintable(n);
                EXPECT_EQ(b.definitions[i].kind, one.definitions[i].kind) << qPrintable(n);
                EXPECT_EQ(b.definitions[i].lang, one.definitions[i].lang) << qPrintable(n);
                EXPECT_EQ(b.definitions[i].signature,
                          one.definitions[i].signature) << qPrintable(n);
            }
        }
    }

    // The equivalence above is vacuous if nothing resolved. These are the
    // needles the fixture exists to resolve.
    SymbolQuery::Options o;
    const auto batch = SymbolQuery::findDefinitions(root, needles, o);
    for (const char *n : {"parseLine", "parseLineExtra", "m_count",
                          "m_countTotal", "load", "loader", "run", "runAll",
                          "deploy"})
        EXPECT_FALSE(batch.value(QString::fromUtf8(n)).definitions.isEmpty())
            << "fixture needle resolved nowhere: " << qPrintable(n);
    EXPECT_FALSE(batch.value(QStringLiteral("1bad")).ok) << "invalid needle";
    EXPECT_EQ(batch.value(QStringLiteral("1bad")).code,
              QStringLiteral("bad_args"));
}

// ANTS-4821 — a brace that INITIALISES a declarator is not a body, so the
// two spellings of one member must agree. The guards below are the forms
// that make the naive "any `{` is a body" rule tempting: each keeps its
// `definition` because a parameter list, a capture list or a class-key
// precedes the brace.
TEST(SymbolQueryCppForms, Ants4821BraceInitIsADeclaration) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;

    for (const char *name : {"m_claudeReviewBtn",  // T *m_p{nullptr};
                             "m_pairs",            // nested `{{1, 2}}`
                             "m_sizes"})           // array extent then `{`
        EXPECT_TRUE(hasKindAt(SymbolQuery::findDefinition(
                                  root, QString::fromUtf8(name), o),
                              "members.h", "declaration"))
            << "brace-initialised member tagged a definition: " << name;

    // Guards: bodies that also end the line in `;`.
    EXPECT_TRUE(hasKindAt(
        SymbolQuery::findDefinition(root, QStringLiteral("Point"), o),
        "members.h", "definition")) << "struct Point { int x; int y; };";
    EXPECT_TRUE(hasKindAt(
        SymbolQuery::findDefinition(root, QStringLiteral("makeFieldsProp"), o),
        "seam.cpp", "definition")) << "lambda with a parameter list";
    EXPECT_TRUE(hasKindAt(
        SymbolQuery::findDefinition(root, QStringLiteral("makeRawProp"), o),
        "seam.cpp", "definition")) << "lambda with no parameter list: `[]{`";
}

// A member whose type carries a COMMA in its template arguments. The same
// shape ANTS-3746 had to fix for return types, and it fails by the same
// route: a comma is neither a type character nor a separator.
TEST(SymbolQueryCppForms, Ants3668CommaBearingMemberTypeResolves) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;
    EXPECT_TRUE(hasKindAt(SymbolQuery::findDefinition(
                              root, QStringLiteral("m_byCategory"), o),
                          "members.h", "declaration"))
        << "QMap<QString, QStringList> m_byCategory;";
}

// The guard that stops the member pattern swallowing call sites. A name that
// is only ever DECLARED as a function and CALLED must not gain a member-shaped
// definition from either line — the pattern requires no `(` before its
// terminator, and this is what proves that requirement is load-bearing.
TEST(SymbolQueryCppForms, Ants3668CallSiteIsNotAMemberDefinition) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = cppFormsRoot(tmp);
    SymbolQuery::Options o;
    const auto d =
        SymbolQuery::findDefinition(root, QStringLiteral("onlyEverCalled"), o);
    bool inCpp = false;
    for (const auto &m : d.definitions)
        if (m.file.endsWith(QLatin1String("members.cpp"))) inCpp = true;
    EXPECT_FALSE(inCpp)
        << "the call site in members.cpp must not read as a definition";
    EXPECT_TRUE(hasKindAt(d, "members.h", "declaration"))
        << "precondition: its real prototype still resolves";
}

// ---------------------------------------------------------------------------
// ANTS-4880 — a wrapped local initialisation is not a definition.
//
// Reported in-session: find_definition {symbol:"counterPath"} returned the
// real `QString counterPath(const QString &)` alongside two rows whose
// signature is `const QString counterPath =` — function-local variables, each
// tagged `definition` and so indistinguishable from the real home.
//
// The ladder matching a local is a DECISION, not an oversight: it is
// line-based with no scope tracking, and reporting where a name is declared
// beats reporting that a visible name does not exist. The existing kind logic
// already tags a local `declaration`, because the single-line form ends in
// `;`. What escaped is the WRAPPED form — an initialiser continued on the next
// line ends in `=`, which looksLikeDeclaration does not recognise, so it fell
// through to `definition`. So this is not a new policy; it is the existing one
// applied to the shape that slipped past it.

TEST(McpSymbolQuery, Ants4880WrappedLocalInitIsNotADefinition) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // The real home: a function definition.
    writeFile(root, QStringLiteral("src/home.cpp"),
              QStringLiteral(
                  "#include <QString>\n"
                  "QString counterPath(const QString &projectPath) {\n"
                  "    return projectPath + QStringLiteral(\"/.counter\");\n"
                  "}\n"));
    // Two impostors: locals whose initialiser wraps onto the next line.
    writeFile(root, QStringLiteral("src/user_a.cpp"),
              QStringLiteral(
                  "#include <QString>\n"
                  "void useA(const QString &root) {\n"
                  "    const QString counterPath =\n"
                  "        root + QStringLiteral(\"/.roadmap-counter\");\n"
                  "    (void)counterPath;\n"
                  "}\n"));
    // And the single-line form, which the existing logic already handles —
    // kept so a fix cannot quietly change it.
    writeFile(root, QStringLiteral("src/user_b.cpp"),
              QStringLiteral(
                  "#include <QString>\n"
                  "void useB(const QString &root) {\n"
                  "    const QString counterPath = root + QStringLiteral(\"/x\");\n"
                  "    (void)counterPath;\n"
                  "}\n"));

    SymbolQuery::Options opts;
    const auto d = SymbolQuery::findDefinition(
        root, QStringLiteral("counterPath"), opts);

    EXPECT_TRUE(hasDef(d, QStringLiteral("src/home.cpp"),
                       QStringLiteral("definition")))
        << "the real function definition must still be found";
    EXPECT_FALSE(hasDef(d, QStringLiteral("src/user_a.cpp"),
                        QStringLiteral("definition")))
        << "a local whose initialiser wraps is a variable, not a second home "
           "for the symbol — this is the reported defect";
    EXPECT_FALSE(hasDef(d, QStringLiteral("src/user_b.cpp"),
                        QStringLiteral("definition")))
        << "the single-line form was already correct and must stay so";

    // The rows are not dropped — the ladder reports where a name is declared
    // on purpose. They are just no longer claiming to be the definition.
    EXPECT_TRUE(hasDef(d, QStringLiteral("src/user_a.cpp"),
                       QStringLiteral("declaration")))
        << "the local should still be reported, as a declaration";
}
