// ANTS-5602 — source_scrapes. Contract: spec.md beside this file.

#include <gtest/gtest.h>

#include "buildtargets.h"
#include "sourcescrapes.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>

namespace {

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

const QByteArray kCMake =
    "set(LIST_REL src/a.cpp src/b.cpp)\n"
    "list(TRANSFORM LIST_REL PREPEND \"${CMAKE_SOURCE_DIR}/\" OUTPUT_VARIABLE _abs)\n"
    "list(JOIN _abs \";\" _joined)\n"
    "target_compile_definitions(t PRIVATE\n"
    "    SRC_A_PATH=\"${CMAKE_SOURCE_DIR}/src/a.cpp\"  # a comment\n"
    "    SRC_B_PATH=\"${CMAKE_SOURCE_DIR}/src/b.cpp\"\n"
    "    ROOT_DIR=\"${CMAKE_SOURCE_DIR}\"\n"
    "    \"LIST_SOURCES=\\\"${_joined}\\\"\")\n";

const QByteArray kHelper =
    "#pragma once\n"
    "namespace t {\n"
    "inline std::string slurpAll() {\n"
    "    std::string out = slurpList(LIST_SOURCES);\n"
    "    return out;\n"
    "}\n"
    "}\n";

// Line numbers are pinned by the tests below.
const QByteArray kOne =
    "#include \"h.h\"\n"                                                     // 1
    "TEST(S, One) {\n"                                                        // 2
    "    const std::string src = slurpFile(SRC_A_PATH);\n"                    // 3
    "    EXPECT_NE(src.find(\"int a(\"), npos);\n"                            // 4
    "    EXPECT_NE(src.find(\"say \\\"hi\\\"\"), npos);\n"                    // 5
    "}\n"                                                                     // 6
    "TEST(S, List) {\n"                                                       // 7
    "    const std::string all = slurpList(LIST_SOURCES);\n"                  // 8
    "    EXPECT_NE(all.find(\"bee\"), npos);\n"                               // 9
    "}\n"                                                                     // 10
    "TEST(S, Dir) {\n"                                                        // 11
    "    const std::string d = slurpFile(std::string(ROOT_DIR) + \"/src/b.cpp\");\n" // 12
    "    EXPECT_NE(d.find(\"dee\"), npos);\n"                                 // 13
    "    const std::string fixture = \"src/zzz.cpp\";\n"                      // 14
    "    EXPECT_NE(fixture.find(\"zed\"), npos);\n"                           // 15
    "}\n"                                                                     // 16
    "TEST(S, ScopeA) {\n"                                                     // 17
    "    const std::string src = slurpFile(SRC_A_PATH);\n"                    // 18
    "    const std::string body = slurpFunctionBody(src, \"void f(\");\n"     // 19
    "    EXPECT_NE(body.find(\"why\"), npos);\n"                              // 20
    "}\n"                                                                     // 21
    "TEST(S, ScopeB) {\n"                                                     // 22
    "    const std::string src = slurpFile(SRC_B_PATH);\n"                    // 23
    "    EXPECT_NE(src.find(\"bonly\"), npos);\n"                             // 24
    "}\n"                                                                     // 25
    "TEST(S, Regex) {\n"                                                      // 26
    "    const std::string src = slurpFile(SRC_A_PATH);\n"                    // 27
    "    const std::regex re(R\"(a\\s*\\()\");\n"                             // 28
    "    EXPECT_TRUE(std::regex_search(src, re));\n"                          // 29
    "    const QString q = QString::fromStdString(src);\n"                    // 30
    "    EXPECT_TRUE(q.contains(QRegularExpression(QStringLiteral(\"b+\"))));\n" // 31
    "}\n"                                                                     // 32
    "TEST(S, Noise) {\n"                                                      // 33
    "    // slurpFile(SRC_A_PATH).find(\"commented\")\n"                      // 34
    "    const std::string s = \"SRC_A_PATH\";\n"                             // 35
    "    EXPECT_NE(s.find(\"strung\"), npos);\n"                              // 36
    "    const std::string src = slurpFile(SRC_A_PATH);\n"                    // 37
    "    for (const char *n : {\"x\"}) EXPECT_NE(src.find(n), npos);\n"       // 38
    "    const std::string call = \"held(\";\n"                               // 39
    "    EXPECT_NE(src.find(call), npos);\n"                                   // 40
    "}\n";                                                                    // 41

const QByteArray kTwo =
    "TEST(T, Helper) {\n"                                                     // 1
    "    EXPECT_NE(t::slurpAll().find(\"hat\"), npos);\n"                     // 2
    "}\n";                                                                    // 3

QTemporaryDir *fixture() {
    auto *dir = new QTemporaryDir;
    writeFile(dir->filePath("CMakeLists.txt"), kCMake);
    writeFile(dir->filePath("src/a.cpp"), "int a() { return 0; }\n");
    writeFile(dir->filePath("src/b.cpp"), "int b() { return 0; }\n");
    writeFile(dir->filePath("tests/_support/h.h"), kHelper);
    writeFile(dir->filePath("tests/one/test_one.cpp"), kOne);
    writeFile(dir->filePath("tests/two/test_two.cpp"), kTwo);
    return dir;
}

QJsonObject call(const QTemporaryDir &dir, const QJsonObject &args) {
    return SourceScrapes::run(QFileInfo(dir.path()).canonicalFilePath(), args);
}

QJsonObject forPath(const QTemporaryDir &dir, const char *path) {
    return call(dir, {{QStringLiteral("path"), QString::fromLatin1(path)}});
}

// The row for `anchor`, or empty.
QJsonObject row(const QJsonObject &r, const QString &anchor) {
    for (const QJsonValue v : r.value(QStringLiteral("scrapes")).toArray())
        if (v.toObject().value(QStringLiteral("anchor")).toString() == anchor) return v.toObject();
    return {};
}

} // namespace

TEST(SourceScrapes, Inv1DefinitionsReadTheirPaths) {
    const QHash<QString, QStringList> defs = BuildTargets::pathDefinitions(QString::fromUtf8(kCMake));
    EXPECT_EQ(defs.value(QStringLiteral("SRC_A_PATH")), QStringList{QStringLiteral("src/a.cpp")});
    EXPECT_EQ(defs.value(QStringLiteral("LIST_SOURCES")),
              (QStringList{QStringLiteral("src/a.cpp"), QStringLiteral("src/b.cpp")}));
    EXPECT_EQ(defs.value(QStringLiteral("ROOT_DIR")), QStringList{QString()});

    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject a = forPath(*dir, "src/a.cpp");
    ASSERT_TRUE(a.value(QStringLiteral("ok")).toBool()) << QJsonDocument(a).toJson().constData();
    const QJsonObject one = row(a, QStringLiteral("int a("));
    EXPECT_EQ(one.value(QStringLiteral("path")).toString(), QStringLiteral("src/a.cpp"));
    EXPECT_EQ(one.value(QStringLiteral("test")).toString(), QStringLiteral("S.One"));
    EXPECT_EQ(one.value(QStringLiteral("file")).toString(), QStringLiteral("tests/one/test_one.cpp"));
    EXPECT_EQ(one.value(QStringLiteral("line")).toInt(), 4);
    EXPECT_EQ(one.value(QStringLiteral("kind")).toString(), QStringLiteral("literal"));
    EXPECT_FALSE(row(a, QStringLiteral("say \\\"hi\\\"")).isEmpty()) << "an anchor is verbatim";

    const QJsonObject b = forPath(*dir, "src/b.cpp");
    EXPECT_EQ(row(b, QStringLiteral("bee")).value(QStringLiteral("line")).toInt(), 9);
}

TEST(SourceScrapes, Inv2DirectoryDefinitionAndLiteral) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject b = forPath(*dir, "src/b.cpp");
    EXPECT_EQ(row(b, QStringLiteral("dee")).value(QStringLiteral("test")).toString(),
              QStringLiteral("S.Dir"));
    const QJsonObject all = forPath(*dir, ".");
    EXPECT_TRUE(row(all, QStringLiteral("zed")).isEmpty()) << "src/zzz.cpp does not exist";
}

TEST(SourceScrapes, Inv3HeaderHelperReads) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject b = forPath(*dir, "src/b.cpp");
    const QJsonObject hat = row(b, QStringLiteral("hat"));
    EXPECT_EQ(hat.value(QStringLiteral("file")).toString(), QStringLiteral("tests/two/test_two.cpp"));
    EXPECT_EQ(hat.value(QStringLiteral("test")).toString(), QStringLiteral("T.Helper"));
}

TEST(SourceScrapes, Inv4BindingsAreScopedAndFollowed) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject a = forPath(*dir, "src/a.cpp");
    EXPECT_FALSE(row(a, QStringLiteral("void f(")).isEmpty());
    EXPECT_EQ(row(a, QStringLiteral("why")).value(QStringLiteral("test")).toString(),
              QStringLiteral("S.ScopeA")) << "body is bound through src";
    EXPECT_TRUE(row(a, QStringLiteral("bonly")).isEmpty()) << "ScopeB's src reads b.cpp only";
    const QJsonObject b = forPath(*dir, "src/b.cpp");
    EXPECT_EQ(row(b, QStringLiteral("bonly")).value(QStringLiteral("test")).toString(),
              QStringLiteral("S.ScopeB"));
}

TEST(SourceScrapes, Inv5LiteralAndRegexAnchors) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject a = forPath(*dir, "src/a.cpp");
    const QJsonObject re = row(a, QStringLiteral("a\\s*\\("));
    EXPECT_EQ(re.value(QStringLiteral("kind")).toString(), QStringLiteral("regex"));
    EXPECT_EQ(re.value(QStringLiteral("line")).toInt(), 28);
    EXPECT_EQ(row(a, QStringLiteral("b+")).value(QStringLiteral("kind")).toString(),
              QStringLiteral("regex"));
}

TEST(SourceScrapes, Inv6CommentsStringsAndVariables) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject a = forPath(*dir, "src/a.cpp");
    EXPECT_TRUE(row(a, QStringLiteral("commented")).isEmpty());
    EXPECT_TRUE(row(a, QStringLiteral("strung")).isEmpty());
    EXPECT_EQ(a.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("unresolved_anchors")).toInt(), 1);
    EXPECT_EQ(row(a, QStringLiteral("held(")).value(QStringLiteral("line")).toInt(), 39)
        << "a variable holding a literal is that literal";
}

TEST(SourceScrapes, Inv7FilteredAndOrdered) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject r = call(*dir, {{QStringLiteral("paths"),
                                       QJsonArray{QStringLiteral("src/b.cpp"), QStringLiteral("src/a.cpp")}}});
    const QJsonArray rows = r.value(QStringLiteral("scrapes")).toArray();
    ASSERT_FALSE(rows.isEmpty());
    QString lastKey;
    for (const QJsonValue v : rows) {
        const QJsonObject o = v.toObject();
        const QString key = QStringLiteral("%1|%2|%3")
                                .arg(o.value(QStringLiteral("path")).toString(),
                                     o.value(QStringLiteral("file")).toString())
                                .arg(o.value(QStringLiteral("line")).toInt(), 4, 10, QLatin1Char('0'));
        EXPECT_LE(lastKey, key);
        lastKey = key;
    }
    EXPECT_EQ(rows.first().toObject().value(QStringLiteral("path")).toString(), QStringLiteral("src/a.cpp"));

    const QJsonObject b = forPath(*dir, "src/b.cpp");
    for (const QJsonValue v : b.value(QStringLiteral("scrapes")).toArray())
        EXPECT_EQ(v.toObject().value(QStringLiteral("path")).toString(), QStringLiteral("src/b.cpp"));
    bool helperRead = false;
    for (const QJsonValue v : b.value(QStringLiteral("readers")).toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("via")).toString() == QLatin1String("slurpAll"))
            helperRead = o.value(QStringLiteral("test")).toString() == QLatin1String("T.Helper");
    }
    EXPECT_TRUE(helperRead) << QJsonDocument(b).toJson().constData();

    const QJsonObject capped = call(*dir, {{QStringLiteral("path"), QStringLiteral("src/a.cpp")},
                                           {QStringLiteral("limit"), 1}});
    EXPECT_EQ(capped.value(QStringLiteral("scrapes")).toArray().size(), 1);
    EXPECT_GT(capped.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("scrapes_total")).toInt(), 1);
    EXPECT_TRUE(capped.value(QStringLiteral("scanned")).toObject()
                    .value(QStringLiteral("rows_truncated")).toBool());
    EXPECT_FALSE(b.value(QStringLiteral("scanned")).toObject()
                     .value(QStringLiteral("rows_truncated")).toBool());
}

TEST(SourceScrapes, Inv8Refusals) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const auto code = [&](const QJsonObject &args) {
        return call(*dir, args).value(QStringLiteral("code")).toString();
    };
    EXPECT_EQ(code({}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{1}}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("path"), QStringLiteral("src/a.cpp")}, {QStringLiteral("limit"), 0}}),
              QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("path"), QStringLiteral("../outside")}}), QStringLiteral("bad_path"));
    EXPECT_EQ(code({{QStringLiteral("path"), QStringLiteral("missing.cpp")}}), QStringLiteral("bad_path"));
}

namespace {

// The reader rows of `path` from `file`.
QList<QJsonObject> readersFrom(const QJsonObject &r, const QString &file) {
    QList<QJsonObject> out;
    for (const QJsonValue v : r.value(QStringLiteral("readers")).toArray())
        if (v.toObject().value(QStringLiteral("file")).toString() == file) out.append(v.toObject());
    return out;
}

QTemporaryDir *scriptFixture() {
    auto *dir = new QTemporaryDir;
    writeFile(dir->filePath("src/pkg/__init__.py"), "");
    writeFile(dir->filePath("src/pkg/store.py"), "x = 1\n");
    writeFile(dir->filePath("src/pkg/words.py"), "y = 1\n");
    writeFile(dir->filePath("src/pkg/plain.py"), "z = 1\n");
    writeFile(dir->filePath("src/pkg/alt.py"), "w = 1\n");
    writeFile(dir->filePath("src/pkg/Bundle.h"), "int b;\n");
    writeFile(dir->filePath("scripts/gate.sh"), "true\n");
    writeFile(dir->filePath("tools/nested.sh"), "true\n");
    writeFile(dir->filePath("tests/test_py.py"),
              "import inspect\n"                                                      // 1
              "from pathlib import Path\n"                                            // 2
              "import pkg.store as store_module\n"                                    // 3
              "from pkg import (\n"                                                   // 4
              "    words,  # a comment\n"                                             // 5
              ")\n"                                                                   // 6
              "from pkg.plain import helper\n"                                        // 7
              "ROOT = Path(__file__).resolve().parent.parent\n"                       // 8
              "_SRC = Path(__file__).resolve().parents[1] / \"src\" / \"pkg\"\n"      // 9
              "def test_getsource():\n"                                               // 10
              "    inspect.getsource(store_module)\n"                                 // 11
              "    inspect.getsource(words)\n"                                        // 12
              "def test_function_source():\n"                                         // 13
              "    inspect.getsource(helper)\n"                                       // 14
              "def test_join(tmp_path):\n"                                            // 15
              "    (ROOT / \"scripts\" / \"gate.sh\").read_text()\n"                  // 16
              "    (_SRC / 'Bundle.h').read_text()\n"                                 // 17
              "def test_noise(tmp_path):\n"                                           // 18
              "    (tmp_path / \"src\" / \"pkg\" / \"store.py\").write_text(\"\")\n"  // 19
              "    # ROOT / \"tools\" / \"nested.sh\"\n"                              // 20
              "    \"\"\"\n"                                                          // 21
              "    ROOT / \"tools\" / \"nested.sh\"\n"                                // 22
              "    \"\"\"\n"                                                          // 23
              "    inspect.getsource(unknown_module)\n"                               // 24
              "def test_dunder_file():\n"                                             // 25
              "    Path(words.__file__).read_text(encoding=\"utf-8\")\n"              // 26
              "def test_method():\n"                                                  // 27
              "    inspect.getsource(helper.method)\n"                                // 28
              "ALT = pathlib.Path(__file__).parent.parent / \"src\"\n"                // 29
              "(ALT / \"pkg\" / \"alt.py\").read_text()\n");                         // 30
    writeFile(dir->filePath("tests/guard/test_guard.sh"),
              "#!/bin/bash\n"                                                         // 1
              "SRC=$1\n"                                                              // 2
              "sed -n 1p \"$SRC/src/pkg/Bundle.h\"\n"                                 // 3
              "here=\"$(cd \"$(dirname \"${BASH_SOURCE[0]}\")\" && pwd)\"\n"          // 4
              "TOP=\"$(cd \"$here/../..\" && pwd)\"\n"                                // 5
              "cp \"$TOP/scripts/gate.sh\" /dev/null\n"                               // 6
              "# cp \"$TOP/tools/nested.sh\"\n"                                       // 7
              "T=$(mktemp -d)\n"                                                      // 8
              "cp \"$T/tools/nested.sh\" /dev/null\n"                                 // 9
              "helper() {\n"                                                          // 10
              "    D=$1\n"                                                            // 11
              "    cat \"$D/tools/nested.sh\"\n"                                      // 12
              "}\n"                                                                   // 13
              "SRC=$(mktemp -d)\n"                                                    // 14
              "cat \"$SRC/tools/nested.sh\"\n");                                      // 15
    return dir;
}

} // namespace

TEST(SourceScrapes, Inv10PythonGetsourceReadsItsModule) {
    std::unique_ptr<QTemporaryDir> dir(scriptFixture());
    const QString file = QStringLiteral("tests/test_py.py");
    const auto reader = [&](const char *path) {
        const QList<QJsonObject> rows = readersFrom(forPath(*dir, path), file);
        return rows.isEmpty() ? QJsonObject() : rows.first();
    };
    const QJsonObject store = reader("src/pkg/store.py");
    EXPECT_EQ(store.value(QStringLiteral("test")).toString(), QStringLiteral("test_getsource"));
    EXPECT_EQ(store.value(QStringLiteral("line")).toInt(), 11);
    EXPECT_EQ(store.value(QStringLiteral("via")).toString(), QStringLiteral("inspect.getsource(store_module)"));
    EXPECT_EQ(reader("src/pkg/words.py").value(QStringLiteral("line")).toInt(), 12)
        << "a name in a bracketed from-import is a module";
    const QList<QJsonObject> words = readersFrom(forPath(*dir, "src/pkg/words.py"), file);
    ASSERT_EQ(words.size(), 2);
    EXPECT_EQ(words.last().value(QStringLiteral("test")).toString(), QStringLiteral("test_dunder_file"));
    EXPECT_EQ(reader("src/pkg/plain.py").value(QStringLiteral("test")).toString(),
              QStringLiteral("test_function_source")) << "a function reads the module holding it";
    const QList<QJsonObject> plain = readersFrom(forPath(*dir, "src/pkg/plain.py"), file);
    ASSERT_EQ(plain.size(), 2);
    EXPECT_EQ(plain.last().value(QStringLiteral("test")).toString(), QStringLiteral("test_method"))
        << "an attribute of an imported name reads the module holding the name";

    QTemporaryDir dotRoot;
    writeFile(dotRoot.filePath(".ants/project.json"), "{\"source_roots\": [\".\"]}\n");
    writeFile(dotRoot.filePath("pkg/m.py"), "x = 1\n");
    writeFile(dotRoot.filePath("tests/test_m.py"),
              "import inspect\nfrom pkg import m\ndef test_m():\n    inspect.getsource(m)\n");
    EXPECT_EQ(readersFrom(forPath(dotRoot, "pkg/m.py"), QStringLiteral("tests/test_m.py")).size(), 1)
        << "a source root of \".\" still gives a root-relative path";
}

TEST(SourceScrapes, Inv11PythonJoinsFromTheTestFile) {
    std::unique_ptr<QTemporaryDir> dir(scriptFixture());
    const QString file = QStringLiteral("tests/test_py.py");
    const QList<QJsonObject> gate = readersFrom(forPath(*dir, "scripts/gate.sh"), file);
    ASSERT_EQ(gate.size(), 1);
    EXPECT_EQ(gate.first().value(QStringLiteral("test")).toString(), QStringLiteral("test_join"));
    EXPECT_EQ(gate.first().value(QStringLiteral("line")).toInt(), 16);
    const QList<QJsonObject> bundle = readersFrom(forPath(*dir, "src/pkg/Bundle.h"), file);
    ASSERT_EQ(bundle.size(), 1) << "a variable holding a directory joins, in either quote";
    EXPECT_EQ(bundle.first().value(QStringLiteral("line")).toInt(), 17);
    const QList<QJsonObject> alt = readersFrom(forPath(*dir, "src/pkg/alt.py"), file);
    ASSERT_EQ(alt.size(), 1) << "pathlib.Path assigned, then joined";
    EXPECT_EQ(alt.first().value(QStringLiteral("line")).toInt(), 30);
    for (const QJsonObject &o : readersFrom(forPath(*dir, "src/pkg/store.py"), file))
        EXPECT_NE(o.value(QStringLiteral("line")).toInt(), 19) << "tmp_path is not the repository";
    EXPECT_TRUE(readersFrom(forPath(*dir, "tools/nested.sh"), file).isEmpty())
        << "a comment or a docstring reads nothing";
}

TEST(SourceScrapes, Inv12ShellJoinsFromAKnownRoot) {
    std::unique_ptr<QTemporaryDir> dir(scriptFixture());
    const QString file = QStringLiteral("tests/guard/test_guard.sh");
    const QList<QJsonObject> bundle = readersFrom(forPath(*dir, "src/pkg/Bundle.h"), file);
    ASSERT_EQ(bundle.size(), 1) << "SRC=$1 at the top level";
    EXPECT_EQ(bundle.first().value(QStringLiteral("line")).toInt(), 3);
    EXPECT_EQ(bundle.first().value(QStringLiteral("via")).toString(), QStringLiteral("$SRC/src/pkg/Bundle.h"));
    const QList<QJsonObject> gate = readersFrom(forPath(*dir, "scripts/gate.sh"), file);
    ASSERT_EQ(gate.size(), 1) << "dirname of BASH_SOURCE, then a cd up from it";
    EXPECT_EQ(gate.first().value(QStringLiteral("line")).toInt(), 6);
    EXPECT_TRUE(readersFrom(forPath(*dir, "tools/nested.sh"), file).isEmpty())
        << "a comment, a mktemp variable, a function's $1 and a rebound name read nothing";
}

TEST(SourceScrapes, Inv13NoticeWhenNoTestFileWasRead) {
    QTemporaryDir empty;
    writeFile(empty.filePath(".ants/project.json"), "{\"test_roots\": [\"tests\"]}\n");
    writeFile(empty.filePath("src/a.js"), "a();\n");
    writeFile(empty.filePath("tests/a.test.js"), "a();\n");
    const QJsonObject r = forPath(empty, "src/a.js");
    EXPECT_EQ(r.value(QStringLiteral("scanned")).toObject().value(QStringLiteral("test_files")).toInt(), 0);
    const QJsonArray notices = r.value(QStringLiteral("notices")).toArray();
    ASSERT_EQ(notices.size(), 1) << QJsonDocument(r).toJson().constData();
    EXPECT_TRUE(notices.first().toString().contains(QStringLiteral("tests")));

    std::unique_ptr<QTemporaryDir> scripts(scriptFixture());
    const QJsonObject read = forPath(*scripts, "src/pkg/store.py");
    EXPECT_EQ(read.value(QStringLiteral("scanned")).toObject().value(QStringLiteral("test_files")).toInt(), 2);
    EXPECT_FALSE(read.contains(QStringLiteral("notices")));
}

TEST(SourceScrapes, Inv9Wiring) {
    const auto slurp = [](const char *rel) {
        std::ifstream f(std::string(ANTS_SRC_DIR) + rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string reg = slurp("/mcptoolregistry.cpp");
    const std::string ci = slurp("/claudeintegration.cpp");
    ASSERT_FALSE(reg.empty());
    ASSERT_FALSE(ci.empty());
    const auto at = reg.find("registerToolProvider(\"source_scrapes\"");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NE(reg.substr(at, 200).find("CallerCwdContract::Required"), std::string::npos);
    const auto scoped = reg.find("registerProjectScopedVerbs");
    ASSERT_NE(scoped, std::string::npos);
    EXPECT_LT(scoped, at);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"source_scrapes\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"source_scrapes\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    const std::string body = ci.substr(schema, end - schema);
    EXPECT_NE(body.find("schema[\"type\"] = \"object\";"), std::string::npos);
    EXPECT_NE(body.find("schema[\"additionalProperties\"] = false;"), std::string::npos);
}

// ANTS-6338 — os.path joins from __file__, and directory walks, are reads.
TEST(SourceScrapes, Ants6338OsPathAndWalksAreReads) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/one.py"), "x = 1\n");
    writeFile(dir.filePath("src/two.py"), "x = 2\n");
    writeFile(dir.filePath("src/deep/three.py"), "x = 3\n");
    writeFile(dir.filePath("src/notes.txt"), "x\n");
    writeFile(dir.filePath("src/__pycache__/one.pyc"), "x\n");
    writeFile(dir.filePath("tools/run.py"), "x = 4\n");
    writeFile(dir.filePath("tests/test_os.py"),
              "import os\n"
              "from pathlib import Path\n"
              "HERE = os.path.dirname(os.path.abspath(__file__))\n"
              "SRC = os.path.join(HERE, '..', 'src')\n"
              "ROOT = Path(__file__).parent.parent\n"
              "\n"
              "def test_join():\n"
              "    open(os.path.join(os.path.dirname(__file__), '..', 'tools', 'run.py')).read()\n"
              "\n"
              "def test_var():\n"
              "    open(os.path.join(SRC, 'one.py')).read()\n"
              "\n"
              "def test_glob():\n"
              "    for p in (ROOT / 'src').glob('*.py'):\n"
              "        p.read_text()\n"
              "\n"
              "def test_walk():\n"
              "    for a, b, c in os.walk(SRC):\n"
              "        pass\n"
              "\n"
              "def test_tree(tmp_path):\n"
              "    open(os.path.join(tmp_path, 'src', 'two.py')).read()\n");
    const QString file = QStringLiteral("tests/test_os.py");
    const auto tests = [&](const char *path) {
        QStringList out;
        for (const QJsonObject &o : readersFrom(forPath(dir, path), file))
            out << o.value(QStringLiteral("test")).toString();
        out.sort();
        return out;
    };
    EXPECT_EQ(tests("tools/run.py"), QStringList{QStringLiteral("test_join")});
    EXPECT_EQ(tests("src/one.py"), (QStringList{QStringLiteral("test_glob"), QStringLiteral("test_var"),
                                                QStringLiteral("test_walk")}));
    EXPECT_EQ(tests("src/two.py"), (QStringList{QStringLiteral("test_glob"), QStringLiteral("test_walk")}))
        << "a tmp_path tree is not the repository";
    EXPECT_EQ(tests("src/deep/three.py"), QStringList{QStringLiteral("test_walk")})
        << "glob stays in its directory; os.walk descends";
    EXPECT_EQ(tests("src/notes.txt"), QStringList{QStringLiteral("test_walk")});
    EXPECT_TRUE(tests("src/__pycache__/one.pyc").isEmpty()) << "a cache directory is not read";
}

// ANTS-6340 — a definition in a test root's own CMakeLists.txt, joined with `/`.
TEST(SourceScrapes, Ants6340NestedDefinitionAndPathChain) {
    QTemporaryDir dir;
    writeFile(dir.filePath("CMakeLists.txt"), "project(x)\n");
    writeFile(dir.filePath("engine/r/render.cpp"), "int r;\n");
    writeFile(dir.filePath("assets/shaders/a.glsl"), "x\n");
    writeFile(dir.filePath("tests/CMakeLists.txt"),
              "add_executable(t test_r.cpp)\n"
              "target_compile_definitions(t PRIVATE SHADER_DIR=\"${CMAKE_SOURCE_DIR}/assets/shaders\")\n");
    writeFile(dir.filePath("tests/test_r.cpp"),
              "#include <gtest/gtest.h>\n"
              "TEST(R, Reads) {\n"
              "    std::ifstream f(std::filesystem::path(SHADER_DIR) / \"..\" / \"..\" / \"engine\" / \"r\" / \"render.cpp\");\n"
              "}\n");
    const QList<QJsonObject> rows = readersFrom(forPath(dir, "engine/r/render.cpp"), QStringLiteral("tests/test_r.cpp"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().value(QStringLiteral("test")).toString(), QStringLiteral("R.Reads"));
}
