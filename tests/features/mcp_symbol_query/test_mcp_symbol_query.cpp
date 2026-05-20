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

    EXPECT_EQ(0, expect_failures());
}

TEST(McpSymbolQuery, WiringContract) {
    expect_reset();

    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);

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
