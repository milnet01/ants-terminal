// Feature-conformance test for ANTS-5313 — doc_symbols mode:"locator".
// Behavioural for the reduction (DocSymbols::locate) and the JSON builder
// (docSymbolsBuildLocatorResponse), both pure. INV-7 scrapes the handler and
// the schema, which need a live MainWindow to reach. See spec.md.

#include "remotecontrol.h"
#include "docsymbols.h"

#include "../../_support/expect.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

ANTS_TEST_SCOPE();

namespace {

using R = DocSymbols::Resolution;

SymbolQuery::DefMatch def(const char *file, int line, const char *kind,
                          const char *sig = "") {
    SymbolQuery::DefMatch d;
    d.signature = QString::fromUtf8(sig);
    d.file = QString::fromUtf8(file);
    d.line = line;
    d.kind = QString::fromUtf8(kind);
    d.lang = QStringLiteral("cpp");
    return d;
}

DocSymbols::Symbol sym(const char *name, R res,
                       const QVector<SymbolQuery::DefMatch> &defs = {}) {
    static int line = 0;
    DocSymbols::Symbol s;
    s.symbol      = QString::fromUtf8(name);
    s.docLine     = ++line;
    s.docCol      = 1;
    s.resolution  = res;
    s.definitions = defs;
    return s;
}

QVector<DocSymbols::Symbol> fixture() {
    return {
        sym("oneDef", R::Resolved, {def("src/a.cpp", 10, "definition")}),
        sym("oneDef", R::Resolved, {def("src/a.cpp", 10, "definition")}),
        sym("declAndDef", R::Resolved, {def("src/b.h", 5, "declaration"),
                                        def("src/b.cpp", 40, "definition")}),
        sym("twoDefs", R::Resolved, {def("src/c.cpp", 1, "definition"),
                                     def("src/d.cpp", 2, "definition")}),
        sym("oneDecl", R::Resolved, {def("src/e.h", 7, "declaration")}),
        sym("twoDecls", R::Resolved, {def("src/f.h", 1, "declaration"),
                                      def("src/g.h", 2, "declaration")}),
        sym("dupMatch", R::Resolved, {def("src/h.cpp", 3, "definition"),
                                      def("src/h.cpp", 3, "definition")}),
        // ANTS-5313 — rows the resolver reports but that are no place to
        // point a reader: a function-local name and a forward declaration.
        sym("defAndLocal", R::Resolved, {def("src/i.cpp", 9, "definition"),
                                         def("src/j.cpp", 4, "local"),
                                         def("src/k.cpp", 5, "local")}),
        sym("memberAndLocal", R::Resolved, {def("src/l.h", 3, "declaration", "int memberAndLocal;"),
                                            def("tests/t.cpp", 8, "local")}),
        sym("localOnly", R::Resolved, {def("tests/t.cpp", 2, "local")}),
        sym("QFile", R::Resolved, {def("src/m.h", 1, "declaration", "class QFile;"),
                                   def("src/n.h", 2, "declaration", "class QFile;")}),
        sym("gone", R::Unresolved),
        sym("gone", R::Unresolved),
        sym("unasked", R::NotChecked),
    };
}

QJsonObject build() {
    return RemoteControl::docSymbolsBuildLocatorResponse(
        DocSymbols::locate(fixture()), /*truncated=*/false,
        QStringList{QStringLiteral("docs/x.md")});
}

}  // namespace

// INV-1 — one entry per distinct symbol, in exactly one bucket.
TEST(DocSymbolsLocator, Inv1OneEntryPerDistinctSymbol) {
    expect_reset();
    const DocSymbols::Locators l = DocSymbols::locate(fixture());
    expect(l.located.size() == 6, "INV-1: six located", QString::number(l.located.size()));
    expect(l.ambiguous.size() == 2, "INV-1: two ambiguous", QString::number(l.ambiguous.size()));
    expect(l.unresolved == QStringList{QStringLiteral("gone")},
           "INV-1: `gone` once in unresolved despite two occurrences");
    expect(l.notChecked == QStringList{QStringLiteral("unasked")},
           "INV-1: `unasked` in not_checked");
    const QJsonObject c = build().value(QStringLiteral("counts")).toObject();
    expect(l.declaredOnly == (QStringList{QStringLiteral("localOnly"), QStringLiteral("QFile")}),
           "INV-1: local-only and forward-only names are declared_only");
    expect(c.value(QStringLiteral("symbols")).toInt() == 12,
           "INV-1: counts.symbols is the distinct count");
    expect(c.value(QStringLiteral("located")).toInt()
               + c.value(QStringLiteral("ambiguous")).toInt()
               + c.value(QStringLiteral("unresolved")).toInt()
               + c.value(QStringLiteral("not_checked")).toInt()
               + c.value(QStringLiteral("declared_only")).toInt()
           == c.value(QStringLiteral("symbols")).toInt(),
           "INV-1: the five buckets sum to counts.symbols");
    EXPECT_EQ(0, expect_finish());
}

// INV-2 — a definition beats a declaration.
TEST(DocSymbolsLocator, Inv2DefinitionBeatsDeclaration) {
    expect_reset();
    const DocSymbols::Locators l = DocSymbols::locate(fixture());
    expect(l.located.value(QStringLiteral("declAndDef")) == QLatin1String("src/b.cpp:40"),
           "INV-2: the .cpp definition, not the header declaration");
    expect(l.located.value(QStringLiteral("oneDef")) == QLatin1String("src/a.cpp:10"),
           "INV-2: a lone definition is its own locator");
    EXPECT_EQ(0, expect_finish());
}

// INV-3 — more than one candidate is ambiguous with a count.
TEST(DocSymbolsLocator, Inv3MultipleCandidatesAreAmbiguous) {
    expect_reset();
    const DocSymbols::Locators l = DocSymbols::locate(fixture());
    expect(l.ambiguous.value(QStringLiteral("twoDefs")) == 2,
           "INV-3: two definitions → ambiguous:2");
    expect(!l.located.contains(QStringLiteral("twoDefs")),
           "INV-3: no guessed locator for twoDefs");
    expect(l.located.value(QStringLiteral("oneDecl")) == QLatin1String("src/e.h:7"),
           "INV-3: a lone declaration locates when no definition exists");
    expect(l.ambiguous.value(QStringLiteral("twoDecls")) == 2,
           "INV-3: two declarations and no definition → ambiguous:2");
    EXPECT_EQ(0, expect_finish());
}

// INV-4 — identical file:line matches collapse to one candidate.
TEST(DocSymbolsLocator, Inv4DuplicateMatchesCollapse) {
    expect_reset();
    const DocSymbols::Locators l = DocSymbols::locate(fixture());
    expect(l.located.value(QStringLiteral("dupMatch")) == QLatin1String("src/h.cpp:3"),
           "INV-4: a repeated match is one candidate, so it locates");
    EXPECT_EQ(0, expect_finish());
}

// INV-8 — a local or a forward declaration is never a candidate.
TEST(DocSymbolsLocator, Inv8LocalsAndForwardDeclarationsAreNotCandidates) {
    expect_reset();
    const DocSymbols::Locators l = DocSymbols::locate(fixture());
    expect(l.located.value(QStringLiteral("defAndLocal")) == QLatin1String("src/i.cpp:9"),
           "INV-8: two locals do not make a definition ambiguous");
    expect(l.located.value(QStringLiteral("memberAndLocal")) == QLatin1String("src/l.h:3"),
           "INV-8: a member declaration beats a test-local of the same name");
    expect(!l.located.contains(QStringLiteral("QFile"))
               && !l.ambiguous.contains(QStringLiteral("QFile")),
           "INV-8: forward declarations locate nothing");
    const QJsonArray d = build().value(QStringLiteral("declared_only")).toArray();
    expect(d.size() == 2, "INV-8: declared_only on the wire");
    EXPECT_EQ(0, expect_finish());
}

// INV-5 / INV-6 — the wire shape.
TEST(DocSymbolsLocator, Inv56WireShape) {
    expect_reset();
    const QJsonObject o = build();
    expect(o.value(QStringLiteral("ok")).toBool(), "ok:true");
    expect(o.value(QStringLiteral("mode")).toString() == QLatin1String("locator"),
           "mode echoed");
    expect(o.value(QStringLiteral("locators")).toObject()
               .value(QStringLiteral("declAndDef")).toString() == QLatin1String("src/b.cpp:40"),
           "locators is a symbol → file:line map");
    expect(o.value(QStringLiteral("ambiguous")).toObject()
               .value(QStringLiteral("twoDefs")).toInt() == 2,
           "ambiguous is a symbol → count map");
    const QJsonArray nc = o.value(QStringLiteral("not_checked")).toArray();
    const QJsonArray un = o.value(QStringLiteral("unresolved")).toArray();
    expect(nc.size() == 1 && nc.at(0).toString() == QLatin1String("unasked"),
           "INV-5: the unasked needle is in not_checked");
    expect(!un.contains(QJsonValue(QStringLiteral("unasked"))),
           "INV-5: and not in unresolved");
    expect(!o.contains(QStringLiteral("symbols")), "INV-6: no symbols[]");
    expect(!o.contains(QStringLiteral("findings")), "INV-6: no findings[]");
    EXPECT_EQ(0, expect_finish());
}

// INV-7 — refusal wiring and schema, scraped.
TEST(DocSymbolsLocator, Inv7RefusalsAndSchema) {
    expect_reset();
    const std::string rc = ants_test::slurpRemoteControl();
    expect(rc.find("ANTS-5313-INV-7") != std::string::npos,
           "INV-7: the mode validation carries its anchor");
    const std::string body = ants_test::slurpFunctionBody(
        rc, "QJsonDocument RemoteControl::cmdDocSymbols(");
    expect(body.find("docSymbolsBuildLocatorResponse(") != std::string::npos,
           "INV-7: the handler routes mode:locator to the locator builder");
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string desc =
        ants_test::squashWhitespace(ants_test::mcpToolDescriptor(ci, "doc_symbols"));
    expect(desc.find("props[\"mode\"]") != std::string::npos,
           "INV-7: the schema declares mode");
    expect(desc.find("QStringLiteral(\"locator\")") != std::string::npos,
           "INV-7: the mode enum lists locator");
    EXPECT_EQ(0, expect_finish());
}

// INV-9 — a qualifier the document wrote is honoured. `A::b` locates only a
// `b` the source puts inside `A`: its own signature says `A::b`, or a class,
// struct or namespace named `A` encloses it. Measured: `QFileSystemWatcher::
// fileChanged` located at `ClaudeIntegration::fileChanged` (ANTS-5313 record,
// seed 5313 entry 40). End to end through scan(), so the signatures are the
// resolver's own, not hand-written.
TEST(DocSymbolsLocator, Inv9QualifierIsHonoured) {
    expect_reset();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    auto put = [&](const char *rel, const char *text) {
        const QString p = root + QLatin1Char('/') + QString::fromUtf8(rel);
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(text);
    };
    put("src/shapes.h",
        "class Alpha {\n"
        "public:\n"
        "    void fire(int n);\n"
        "};\n"
        "class Beta {\n"
        "    void fire(int n);\n"
        "};\n"
        "namespace ns {\n"
        "int helperFn(int n);\n"
        "}\n");
    put("src/shapes.cpp",
        "#include \"shapes.h\"\n"
        "void Alpha::fire(int n) {\n"
        "    (void)n;\n"
        "}\n");

    DocSymbols::Options o;
    o.rootCanonical = root;
    const DocSymbols::ScanResult r = DocSymbols::scan(QStringLiteral(
        "`Alpha::fire()` `Beta::fire` `Gamma::fire()` `ns::helperFn` `fire()`\n"),
        QStringLiteral("docs/x.md"), o);
    const DocSymbols::Locators l = DocSymbols::locate(r.symbols, [&root](const QString &rel) {
        QFile f(root + QLatin1Char('/') + rel);
        return f.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))
                   : QStringList();
    });

    expect(l.located.value(QStringLiteral("Alpha::fire()")) == QLatin1String("src/shapes.cpp:2"),
           "INV-9: Alpha's out-of-line definition", l.located.value(QStringLiteral("Alpha::fire()")));
    expect(l.located.value(QStringLiteral("Beta::fire")) == QLatin1String("src/shapes.h:6"),
           "INV-9: only the declaration inside class Beta",
           l.located.value(QStringLiteral("Beta::fire")));
    expect(l.unresolved.contains(QStringLiteral("Gamma::fire()")),
           "INV-9: no `fire` lives in Gamma, so nothing is located");
    expect(l.located.value(QStringLiteral("ns::helperFn")) == QLatin1String("src/shapes.h:9"),
           "INV-9: a namespace qualifier", l.located.value(QStringLiteral("ns::helperFn")));
    expect(l.located.value(QStringLiteral("fire()")) == QLatin1String("src/shapes.cpp:2"),
           "INV-9: an unqualified name is unaffected (the one definition wins)");
    EXPECT_EQ(0, expect_finish());
}
