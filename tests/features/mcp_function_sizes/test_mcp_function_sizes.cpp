// ANTS-5787 — function_sizes. Contract: spec.md beside this file.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "functionsizes.h"

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

QJsonObject call(const QTemporaryDir &dir, QJsonObject args = {}) {
    return FunctionSizes::run(QFileInfo(dir.path()).canonicalFilePath(), args);
}

QJsonObject rowFor(const QJsonObject &r, const QString &symbol) {
    for (const QJsonValue v : r.value(QStringLiteral("functions")).toArray())
        if (v.toObject().value(QStringLiteral("symbol")).toString() == symbol)
            return v.toObject();
    return {};
}

// short_one: 3 lines; long_one: 7 lines with a brace in a string and a
// comment; a declaration with no body.
const QByteArray kCpp =
    "int declared(int x);\n"                  // 1
    "int short_one(int x) {\n"                // 2
    "    return x;\n"                         // 3
    "}\n"                                     // 4
    "int long_one(int x) {\n"                 // 5
    "    const char *s = \"}\"; // }\n"       // 6
    "    if (x) {\n"                          // 7
    "        x++;\n"                          // 8
    "    }\n"                                 // 9
    "    return x;\n"                         // 10
    "}\n";                                    // 11

} // namespace

TEST(FunctionSizes, Inv1RankedLongestFirst) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/a.cpp"), kCpp);
    const QJsonObject r = call(dir);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().constData();
    const QJsonArray rows = r.value(QStringLiteral("functions")).toArray();
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).toObject().value(QStringLiteral("symbol")).toString(),
              QStringLiteral("long_one"));
    EXPECT_EQ(rows.at(0).toObject().value(QStringLiteral("lines")).toInt(), 7);
    EXPECT_EQ(rows.at(1).toObject().value(QStringLiteral("lines")).toInt(), 3);

    const QJsonObject capped = call(dir, {{QStringLiteral("top"), 1}});
    EXPECT_EQ(capped.value(QStringLiteral("functions")).toArray().size(), 1);
    EXPECT_EQ(capped.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("functions_measured")).toInt(), 2);
}

TEST(FunctionSizes, Inv2BraceBodyEndsAtItsBrace) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/a.cpp"), kCpp);
    const QJsonObject r = call(dir);
    const QJsonObject longOne = rowFor(r, QStringLiteral("long_one"));
    EXPECT_EQ(longOne.value(QStringLiteral("body_start")).toInt(), 5);
    EXPECT_EQ(longOne.value(QStringLiteral("body_end")).toInt(), 11);
    EXPECT_EQ(longOne.value(QStringLiteral("file")).toString(), QStringLiteral("src/a.cpp"));
    EXPECT_TRUE(rowFor(r, QStringLiteral("declared")).isEmpty())
        << "a declaration with no body is not a row";
}

TEST(FunctionSizes, Inv2aNoBraceBodyIsUnmeasured) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/k.kt"),
              "fun expr(x: Int) = x + 1\n"   // 1 — no brace body
              "fun braced(x: Int): Int {\n"   // 2
              "    return x\n"                // 3
              "}\n");                         // 4
    writeFile(dir.filePath("src/r.rb"),
              "def ruby_one\n"
              "  1\n"
              "end\n");
    const QJsonObject r = call(dir);
    EXPECT_TRUE(rowFor(r, QStringLiteral("expr")).isEmpty())
        << QJsonDocument(r).toJson().constData();
    EXPECT_EQ(rowFor(r, QStringLiteral("braced")).value(QStringLiteral("body_end")).toInt(), 4);
    EXPECT_TRUE(rowFor(r, QStringLiteral("ruby_one")).isEmpty());
    EXPECT_EQ(r.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("unmeasured")).toInt(), 2);
    const QJsonArray named = r.value(QStringLiteral("unmeasured")).toArray();
    ASSERT_EQ(named.size(), 2);
    QStringList where;
    for (const QJsonValue v : named) {
        const QJsonObject o = v.toObject();
        where << QStringLiteral("%1:%2:%3").arg(o.value(QStringLiteral("file")).toString())
                     .arg(o.value(QStringLiteral("line")).toInt())
                     .arg(o.value(QStringLiteral("symbol")).toString());
        EXPECT_FALSE(o.value(QStringLiteral("why")).toString().isEmpty());
    }
    EXPECT_TRUE(where.contains(QStringLiteral("src/k.kt:1:expr"))) << where.join(' ').toStdString();
    EXPECT_TRUE(where.contains(QStringLiteral("src/r.rb:1:ruby_one"))) << where.join(' ').toStdString();
    EXPECT_EQ(r.value(QStringLiteral("not_covered")).toArray().size(), 1);
}

// ANTS-6352 — a named Python script with no extension is measured, not counted
// as a scanned file holding no functions.
TEST(FunctionSizes, Inv10ExtensionlessPythonScriptIsMeasured) {
    QTemporaryDir dir;
    writeFile(dir.filePath("tools/lane-verify"),
              "#!/usr/bin/env python3\n"  // 1
              "def main():\n"             // 2
              "    return 1\n"            // 3
              "main()\n");                // 4
    const QJsonObject r = call(dir, {{QStringLiteral("paths"),
                                      QJsonArray{QStringLiteral("tools/lane-verify")}}});
    const QJsonObject row = rowFor(r, QStringLiteral("main"));
    EXPECT_EQ(row.value(QStringLiteral("body_end")).toInt(), 3)
        << QJsonDocument(r).toJson().constData();
    EXPECT_EQ(r.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("top_level_lines")).toInt(), 1);
    EXPECT_TRUE(r.value(QStringLiteral("not_covered")).toArray().at(0).toString()
                    .contains(QStringLiteral("extensionless")));
}

// ANTS-5800 — a script's main body is code no function holds, so ranking only
// its small functions read as full coverage.
TEST(FunctionSizes, Inv9TopLevelCodeIsMeasured) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/run.sh"),
              "#!/bin/sh\n"          // 1, a comment
              "echo one\n"           // 2
              "\n"
              "small() {\n"          // 4
              "    echo body\n"
              "}\n"                  // 6
              "# a comment\n"
              "echo two\n"           // 8
              "echo three\n");       // 9
    writeFile(dir.filePath("src/m.py"),
              "import os\n"          // 1
              "def f():\n"           // 2
              "    return 1\n"
              "print(os.name)\n");   // 4
    writeFile(dir.filePath("src/a.cpp"), kCpp);

    const QJsonObject r = call(dir);   // the walk of src reaches the shell file
    const QJsonArray rows = r.value(QStringLiteral("top_level")).toArray();
    ASSERT_EQ(rows.size(), 2) << QJsonDocument(r).toJson().constData();
    const QJsonObject sh = rows.at(0).toObject();
    EXPECT_EQ(sh.value(QStringLiteral("file")).toString(), QStringLiteral("src/run.sh"));
    EXPECT_EQ(sh.value(QStringLiteral("lines")).toInt(), 3);
    EXPECT_EQ(sh.value(QStringLiteral("first_line")).toInt(), 2);
    EXPECT_EQ(sh.value(QStringLiteral("last_line")).toInt(), 9);
    const QJsonObject py = rows.at(1).toObject();
    EXPECT_EQ(py.value(QStringLiteral("file")).toString(), QStringLiteral("src/m.py"));
    EXPECT_EQ(py.value(QStringLiteral("lines")).toInt(), 2);
    EXPECT_EQ(r.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("top_level_lines")).toInt(), 5);
    EXPECT_FALSE(rowFor(r, QStringLiteral("small")).isEmpty());

    const QJsonObject capped = call(dir, {{QStringLiteral("top"), 1}});
    EXPECT_EQ(capped.value(QStringLiteral("top_level")).toArray().size(), 1);
    EXPECT_EQ(capped.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("top_level_lines")).toInt(), 5);
}

TEST(FunctionSizes, Inv3PythonBodyEndsByIndentation) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/m.py"),
              "def first(x):\n"          // 1
              "    if x:\n"              // 2
              "        return 1\n"       // 3
              "\n"                       // 4
              "    # a comment\n"        // 5
              "    return 2\n"           // 6
              "\n"                       // 7
              "\n"                       // 8
              "def second():\n"          // 9
              "    pass\n");             // 10
    const QJsonObject r = call(dir);
    const QJsonObject first = rowFor(r, QStringLiteral("first"));
    ASSERT_FALSE(first.isEmpty()) << QJsonDocument(r).toJson().constData();
    EXPECT_EQ(first.value(QStringLiteral("body_end")).toInt(), 6);
    EXPECT_EQ(first.value(QStringLiteral("lines")).toInt(), 6);
    EXPECT_EQ(first.value(QStringLiteral("nesting_depth")).toInt(), 2);
    EXPECT_EQ(rowFor(r, QStringLiteral("second")).value(QStringLiteral("body_end")).toInt(), 10);
}

TEST(FunctionSizes, Inv4NestingDepth) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/a.cpp"), kCpp);
    const QJsonObject r = call(dir);
    EXPECT_EQ(rowFor(r, QStringLiteral("short_one")).value(QStringLiteral("nesting_depth")).toInt(), 1);
    EXPECT_EQ(rowFor(r, QStringLiteral("long_one")).value(QStringLiteral("nesting_depth")).toInt(), 2);
}

TEST(FunctionSizes, Inv5TestsAndGeneratedLeftOut) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/a.cpp"), kCpp);
    writeFile(dir.filePath("src/moc_a.cpp"), kCpp);
    writeFile(dir.filePath("src/tests/t.cpp"), kCpp);
    writeFile(dir.filePath("src/a_test.cpp"), kCpp);
    const QJsonObject args{{QStringLiteral("paths"), QJsonArray{QStringLiteral("src")}}};
    const QJsonObject r = call(dir, args);
    const QJsonObject scanned = r.value(QStringLiteral("scanned")).toObject();
    EXPECT_EQ(scanned.value(QStringLiteral("files")).toInt(), 1);
    EXPECT_EQ(scanned.value(QStringLiteral("skipped_tests")).toInt(), 2);
    EXPECT_EQ(scanned.value(QStringLiteral("skipped_generated")).toInt(), 1);

    QJsonObject all = args;
    all[QStringLiteral("include_tests")] = true;
    all[QStringLiteral("include_generated")] = true;
    EXPECT_EQ(call(dir, all).value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("files")).toInt(), 4);

    const QJsonObject named{{QStringLiteral("paths"), QJsonArray{QStringLiteral("src/moc_a.cpp")}}};
    EXPECT_EQ(call(dir, named).value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("files")).toInt(), 1)
        << "a file named in paths is measured whatever it is";
}

TEST(FunctionSizes, Inv6EmptyAnswerSaysWhatItScanned) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/notes.cpp"), "// no functions here\n");
    const QJsonObject r = call(dir);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(r.value(QStringLiteral("functions")).toArray().isEmpty());
    const QJsonObject scanned = r.value(QStringLiteral("scanned")).toObject();
    EXPECT_EQ(scanned.value(QStringLiteral("files")).toInt(), 1);
    EXPECT_EQ(scanned.value(QStringLiteral("functions_measured")).toInt(), 0);
    EXPECT_TRUE(scanned.contains(QStringLiteral("unmeasured")));
}

TEST(FunctionSizes, Inv7Refusals) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/a.cpp"), kCpp);
    const auto code = [&](const QJsonObject &args) {
        return call(dir, args).value(QStringLiteral("code")).toString();
    };
    EXPECT_EQ(code({{QStringLiteral("top"), 0}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("top"), 501}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{1}}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{QStringLiteral("../outside")}}}),
              QStringLiteral("bad_path"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{QStringLiteral("missing")}}}),
              QStringLiteral("bad_path"));
}

TEST(FunctionSizes, Inv8Wiring) {
    const auto slurp = [](const char *rel) {
        std::ifstream f(std::string(ANTS_SRC_DIR) + rel);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    const std::string reg = slurp("/mcptoolregistry.cpp");
    const std::string ci = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(reg.empty());
    ASSERT_FALSE(ci.empty());
    const auto at = reg.find("registerToolProvider(\"function_sizes\"");
    ASSERT_NE(at, std::string::npos);
    // ANTS-3681 — the registration entry, bounded by the next one.
    EXPECT_NE(ants_test::regionBetween(reg, "registerToolProvider(\"function_sizes\"",
                                       "registerToolProvider(")
                  .find("CallerCwdContract::Required"),
              std::string::npos);
    const auto scoped = reg.find("registerProjectScopedVerbs");
    ASSERT_NE(scoped, std::string::npos);
    EXPECT_LT(scoped, at);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"function_sizes\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"function_sizes\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    const std::string body = ci.substr(schema, end - schema);
    EXPECT_NE(body.find("schema[\"type\"] = \"object\";"), std::string::npos);
    EXPECT_NE(body.find("schema[\"additionalProperties\"] = false;"), std::string::npos);
}
