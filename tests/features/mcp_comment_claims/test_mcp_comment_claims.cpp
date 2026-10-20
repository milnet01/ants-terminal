// ANTS-5786 — comment_claims. Contract: spec.md beside this file.

#include <gtest/gtest.h>

#include "commentclaims.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <fstream>
#include <memory>
#include <sstream>

namespace {

void writeFile(const QString &path, const QByteArray &bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

// Line numbers are pinned by the tests below.
const QByteArray kA =
    "// src/data.cpp holds \"the quick  brown fox jumps\" as it says.\n"            // 1
    "// src/data.cpp says \"a sentence found nowhere at all\"; \"a phrase near src/data.cpp\".\n" // 2
    "const char *s = \"// see src/missing.cpp ANTS-999\";\n"                       // 3
    "// see docs/gone.md, see `realFunction()`, see ghostFunction(), see addWidget and see the docs.\n" // 4
    "// see QWidget::event(), see helper.h, see MY_MACRO and `see no/such.md`.\n"   // 5
    "// ANTS-2 and ANTS-3 and ANTS-77 and CFG-12.\n"                                // 6
    "/* a ~140-line function\n"                                                     // 7
    " * holds the 5 keys, 3 seconds, 2026-10-19, v1.2 lists and 3, then rows */\n"   // 8
    "int realFunction() { return 0; }\n"                                            // 9
    "// first line of a block\n"                                                    // 10
    "// second line names ANTS-404\n"                                               // 11
    "// two on one line: ANTS-500 ANTS-501\n";                                      // 12

QTemporaryDir *fixture() {
    auto *dir = new QTemporaryDir;
    writeFile(dir->filePath("ROADMAP.md"), "- 📋 [ANTS-1] **One.**\n- ✅ [ANTS-2] **Two.**\n");
    writeFile(dir->filePath("docs/roadmap/0.5.md"), "- ✅ [ANTS-3] **Three.**\n");
    writeFile(dir->filePath("src/data.cpp"), "const char *msg = \"the quick brown fox jumps\";\n");
    writeFile(dir->filePath("src/a.cpp"), kA);
    writeFile(dir->filePath("src/s.py"), "x = \"# not a comment ANTS-600\"\n# ANTS-601 python\n");
    writeFile(dir->filePath("src/s.lua"), "local x = \"-- ANTS-610\" -- ANTS-611 lua\n");
    writeFile(dir->filePath("src/s.sh"), "echo ${#arr} $# # ANTS-620 shell\n");
    writeFile(dir->filePath("tests/test_x.cpp"), "// ANTS-700\n");
    return dir;
}

QJsonObject call(const QTemporaryDir &dir, const QJsonObject &args = {}) {
    return CommentClaims::run(QFileInfo(dir.path()).canonicalFilePath(), args);
}

QList<QJsonObject> rows(const QJsonObject &r, const char *bucket, const QString &kind) {
    QList<QJsonObject> out;
    for (const QJsonValue v : r.value(QLatin1String(bucket)).toArray())
        if (v.toObject().value(QStringLiteral("kind")).toString() == kind) out.append(v.toObject());
    return out;
}

QStringList quotes(const QList<QJsonObject> &rs) {
    QStringList out;
    for (const QJsonObject &o : rs) out.append(o.value(QStringLiteral("quote")).toString());
    return out;
}

} // namespace

TEST(CommentClaims, Inv1CommentsAreHarvested) {
    const QList<CommentClaims::Block> blocks =
        CommentClaims::harvest(QStringLiteral("a.cpp"), QString::fromUtf8(kA));
    for (const CommentClaims::Block &b : blocks)
        for (const CommentClaims::CommentLine &cl : b) EXPECT_NE(cl.line, 3) << "line 3 is a string";
    bool merged = false;
    for (const CommentClaims::Block &b : blocks)
        if (b.size() == 3 && b.at(0).line == 10 && b.at(2).line == 12) merged = true;
    EXPECT_TRUE(merged) << "lines 10 to 12 form one block";

    // Lua and shell are outside the directory walk's languages, so they are
    // named; the Python file is reached by the walk of src.
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject r = call(*dir, {{QStringLiteral("paths"),
                                       QJsonArray{QStringLiteral("src"), QStringLiteral("src/s.lua"),
                                                  QStringLiteral("src/s.sh")}}});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().constData();
    const QStringList t = quotes(rows(r, "findings", QStringLiteral("unknown_ticket")));
    EXPECT_TRUE(t.contains(QStringLiteral("ANTS-601")));
    EXPECT_TRUE(t.contains(QStringLiteral("ANTS-611")));
    EXPECT_TRUE(t.contains(QStringLiteral("ANTS-620")));
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-600")));
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-610")));
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-999")));
}

TEST(CommentClaims, Inv2QuotationMissingFromItsFile) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QList<QJsonObject> q = rows(call(*dir), "findings", QStringLiteral("missing_quotation"));
    ASSERT_EQ(q.size(), 1);
    EXPECT_EQ(q.first().value(QStringLiteral("quote")).toString(),
              QStringLiteral("a sentence found nowhere at all"));
    EXPECT_EQ(q.first().value(QStringLiteral("line")).toInt(), 2);
    EXPECT_TRUE(q.first().value(QStringLiteral("why")).toString().contains(QStringLiteral("src/data.cpp")));
}

TEST(CommentClaims, Inv3DeadReference) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QList<QJsonObject> d = rows(call(*dir), "findings", QStringLiteral("dead_reference"));
    const QStringList q = quotes(d);
    EXPECT_EQ(q.size(), 2) << q.join(QStringLiteral(" | ")).toStdString();
    EXPECT_TRUE(q.filter(QStringLiteral("docs/gone.md")).size() == 1);
    EXPECT_TRUE(q.filter(QStringLiteral("ghostFunction")).size() == 1);
    for (const QJsonObject &o : d) EXPECT_EQ(o.value(QStringLiteral("line")).toInt(), 4);
}

TEST(CommentClaims, Inv4UnknownTicket) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QStringList t = quotes(rows(call(*dir), "findings", QStringLiteral("unknown_ticket")));
    EXPECT_TRUE(t.contains(QStringLiteral("ANTS-77")));
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-2")));
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-3"))) << "an archived id counts";
    EXPECT_FALSE(t.contains(QStringLiteral("CFG-12"))) << "another prefix is not checked";
}

TEST(CommentClaims, Inv5CensusNumbers) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QList<QJsonObject> c = rows(call(*dir), "candidates", QStringLiteral("census_number"));
    const QStringList q = quotes(c);
    EXPECT_EQ(q, (QStringList{QStringLiteral("~140-line"), QStringLiteral("5 keys")}))
        << q.join(QStringLiteral(" | ")).toStdString();
    EXPECT_EQ(c.value(0).value(QStringLiteral("line")).toInt(), 7);
    EXPECT_EQ(c.value(1).value(QStringLiteral("line")).toInt(), 8);
}

TEST(CommentClaims, Inv6StableIds) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject r = call(*dir);
    QStringList ids;
    for (const QJsonObject &o : rows(r, "findings", QStringLiteral("unknown_ticket")))
        ids.append(o.value(QStringLiteral("id")).toString());
    EXPECT_TRUE(ids.contains(QStringLiteral("src/a.cpp:11:unknown_ticket:0")));
    EXPECT_TRUE(ids.contains(QStringLiteral("src/a.cpp:12:unknown_ticket:0")));
    EXPECT_TRUE(ids.contains(QStringLiteral("src/a.cpp:12:unknown_ticket:1")));
    EXPECT_EQ(QJsonDocument(call(*dir)).toJson(), QJsonDocument(r).toJson());
}

TEST(CommentClaims, Inv7ScopeAndCounts) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QJsonObject r = call(*dir, {{QStringLiteral("paths"), QJsonArray{QStringLiteral(".")}}});
    const QJsonObject s = r.value(QStringLiteral("scanned")).toObject();
    EXPECT_EQ(s.value(QStringLiteral("skipped_tests")).toInt(), 1);
    EXPECT_GT(s.value(QStringLiteral("comments")).toInt(), 0);
    EXPECT_EQ(s.value(QStringLiteral("symbols_unchecked")).toInt(), 0);
    EXPECT_FALSE(quotes(rows(r, "findings", QStringLiteral("unknown_ticket"))).contains(QStringLiteral("ANTS-700")));

    const QJsonObject withTests = call(*dir, {{QStringLiteral("paths"), QJsonArray{QStringLiteral(".")}},
                                              {QStringLiteral("include_tests"), true}});
    EXPECT_TRUE(quotes(rows(withTests, "findings", QStringLiteral("unknown_ticket")))
                    .contains(QStringLiteral("ANTS-700")));

    const QJsonObject capped = call(*dir, {{QStringLiteral("limit"), 1}});
    EXPECT_EQ(capped.value(QStringLiteral("findings")).toArray().size(), 1);
    EXPECT_GT(capped.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("findings_total")).toInt(), 1);
    EXPECT_TRUE(capped.value(QStringLiteral("scanned")).toObject()
                    .value(QStringLiteral("rows_truncated")).toBool());
    EXPECT_FALSE(s.value(QStringLiteral("rows_truncated")).toBool());
}

TEST(CommentClaims, Inv8Refusals) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const auto code = [&](const QJsonObject &args) {
        return call(*dir, args).value(QStringLiteral("code")).toString();
    };
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{1}}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("limit"), 0}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("limit"), 5001}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{QStringLiteral("../outside")}}}),
              QStringLiteral("bad_path"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{QStringLiteral("missing")}}}),
              QStringLiteral("bad_path"));
}

TEST(CommentClaims, Inv9Wiring) {
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
    const auto at = reg.find("registerToolProvider(\"comment_claims\"");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NE(reg.substr(at, 200).find("CallerCwdContract::Required"), std::string::npos);
    const auto scoped = reg.find("registerProjectScopedVerbs");
    ASSERT_NE(scoped, std::string::npos);
    EXPECT_LT(scoped, at);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"comment_claims\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"comment_claims\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    const std::string body = ci.substr(schema, end - schema);
    EXPECT_NE(body.find("schema[\"type\"] = \"object\";"), std::string::npos);
    EXPECT_NE(body.find("schema[\"additionalProperties\"] = false;"), std::string::npos);
}
