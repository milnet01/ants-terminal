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
