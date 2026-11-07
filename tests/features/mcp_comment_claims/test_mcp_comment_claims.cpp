// ANTS-5786 — comment_claims. Contract: spec.md beside this file.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

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

// ANTS-6350 — measured on freellmapi: `see lib/alias-merge` names
// client/src/lib/alias-merge.ts, from client/src/pages.
TEST(CommentClaims, Inv3ShorterPathIsNotDead) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    writeFile(dir.filePath("client/lib/alias-merge.ts"), "");
    writeFile(dir.filePath("other/lib/alias-merge.js"), "");
    writeFile(dir.filePath("server/db/pricing.cpp"), "");
    writeFile(dir.filePath("server/db/tables/users.sql"), "");
    writeFile(dir.filePath("build/dep/noise.cpp"), "");
    writeFile(dir.filePath("src/a.cpp"),
              "// see lib/alias-merge\n"        // no extension, two files match
              "// see db/pricing.cpp\n"         // a shorter path
              "// see db/tables\n"              // a directory
              "// see db/ricing.cpp\n"          // part of a segment: dead
              "// see dep/noise.cpp\n"          // a noise directory: dead
              "// see lib/alias-merge.cpp\n");  // the wrong extension: dead
    const QStringList q = quotes(rows(call(dir), "findings", QStringLiteral("dead_reference")));
    EXPECT_EQ(q, (QStringList{QStringLiteral("see db/ricing.cpp"),
                              QStringLiteral("see dep/noise.cpp"),
                              QStringLiteral("see lib/alias-merge.cpp")}))
        << q.join(QStringLiteral(" | ")).toStdString();
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

TEST(CommentClaims, Inv5NumbersThatAreNotCounts) {
    QTemporaryDir dir;
    writeFile(dir.filePath("src/n.cpp"),
              "// §2 expects, § 3 keeps, §§ 4 and 5 holds, SS 6 says.\n"
              "// step 6 raises, Decision 10 refuses, section 7 lists, steps 8, 9 and 11 runs.\n"
              "// a 404 keeps, a 400 names it, $2 holds, UTA-0432 headers.\n"
              "// 90 degrees, 16 bytes, 8 bits, 32 pixels.\n"
              // Each of these is a count, and stays a candidate.
              "// the 7 keys, 404 files, 123 keeps, ~60 KB.\n");
    const QStringList q = quotes(rows(call(dir), "candidates", QStringLiteral("census_number")));
    EXPECT_EQ(q, (QStringList{QStringLiteral("~60 KB"), QStringLiteral("7 keys"),
                              QStringLiteral("404 files"), QStringLiteral("123 keeps")}))
        << q.join(QStringLiteral(" | ")).toStdString();
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

TEST(CommentClaims, Inv10ShellFilesAndUnreadFiles) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    writeFile(dir->filePath("src/c.fish"), "# ANTS-630 fish\n");
    writeFile(dir->filePath("src/c.zsh"), "# ANTS-631 zsh\n");
    writeFile(dir->filePath("src/c.ksh"), "# ANTS-632 ksh\n");
    writeFile(dir->filePath("src/_completion"), "#compdef demoreel\n# ANTS-633 completion\n");
    writeFile(dir->filePath("src/runner"), "#!/usr/bin/env bash\n# ANTS-634 script\n");
    writeFile(dir->filePath("src/notes.txt"), "# ANTS-635 not code\n");
    writeFile(dir->filePath("src/noder"), "#!/usr/bin/env node\n# ANTS-636 another syntax\n");
    QJsonArray paths;
    for (const char *p : {"src/c.fish", "src/c.zsh", "src/c.ksh", "src/_completion", "src/runner",
                          "src/notes.txt", "src/noder"})
        paths.append(QString::fromLatin1(p));
    const QJsonObject r = call(*dir, {{QStringLiteral("paths"), paths}});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().constData();

    const QStringList t = quotes(rows(r, "findings", QStringLiteral("unknown_ticket")));
    for (const char *id : {"ANTS-630", "ANTS-631", "ANTS-632", "ANTS-633", "ANTS-634"})
        EXPECT_TRUE(t.contains(QString::fromLatin1(id))) << id;
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-635")));
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-636")));

    EXPECT_EQ(r.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("unread_files")).toInt(), 2);
    QStringList notCovered;
    for (const QJsonValue v : r.value(QStringLiteral("not_covered")).toArray())
        notCovered.append(v.toString());
    const QString named = notCovered.filter(QStringLiteral("src/notes.txt")).value(0);
    EXPECT_TRUE(named.contains(QStringLiteral("src/noder"))) << notCovered.join('\n').toStdString();
    EXPECT_FALSE(named.contains(QStringLiteral("src/runner")));
    EXPECT_FALSE(notCovered.filter(QStringLiteral("extensionless")).isEmpty())
        << "a directory walk does not reach these, and the reply must say so";

    // A long list is cut, and the count stays whole.
    std::unique_ptr<QTemporaryDir> many(fixture());
    QJsonArray lots;
    for (int i = 0; i < 25; ++i) {
        const QString rel = QStringLiteral("src/n%1.txt").arg(i);
        writeFile(many->filePath(rel), "x\n");
        lots.append(rel);
    }
    const QJsonObject cut = call(*many, {{QStringLiteral("paths"), lots}});
    EXPECT_EQ(cut.value(QStringLiteral("scanned")).toObject()
                  .value(QStringLiteral("unread_files")).toInt(), 25);
    QStringList cutLines;
    for (const QJsonValue v : cut.value(QStringLiteral("not_covered")).toArray())
        cutLines.append(v.toString());
    EXPECT_FALSE(cutLines.filter(QStringLiteral("and 5 more")).isEmpty()) << cutLines.join('\n').toStdString();
}

TEST(CommentClaims, Inv11MoreSlashCommentSuffixes) {
    std::unique_ptr<QTemporaryDir> dir(fixture());
    const QList<const char *> suffixes = {"tsx", "jsx", "mjs", "cjs", "uc", "vsh", "fsh"};
    int id = 640;
    QStringList expected;
    for (const char *s : suffixes) {
        const QString ticket = QStringLiteral("ANTS-%1").arg(id++);
        writeFile(dir->filePath(QStringLiteral("src/m.%1").arg(QLatin1String(s))),
                  QStringLiteral("// %1\n").arg(ticket).toUtf8());
        expected.append(ticket);
    }
    // HTML has no `//` comments, so it stays unread rather than misread.
    writeFile(dir->filePath("src/page.html"), "// ANTS-660 page text\n");
    const QJsonObject r = call(*dir, {{QStringLiteral("paths"), QJsonArray{QStringLiteral("src")}}});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().constData();

    const QStringList t = quotes(rows(r, "findings", QStringLiteral("unknown_ticket")));
    for (const QString &ticket : expected)
        EXPECT_TRUE(t.contains(ticket)) << ticket.toStdString() << "\n"
                                        << QJsonDocument(r).toJson().constData();
    EXPECT_FALSE(t.contains(QStringLiteral("ANTS-660")));
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
    const std::string ci = ants_test::slurpClaudeIntegration();
    ASSERT_FALSE(reg.empty());
    ASSERT_FALSE(ci.empty());
    const auto at = reg.find("registerToolProvider(\"comment_claims\"");
    ASSERT_NE(at, std::string::npos);
    // ANTS-3681 — the registration entry, bounded by the next one.
    EXPECT_NE(ants_test::regionBetween(reg, "registerToolProvider(\"comment_claims\"",
                                       "registerToolProvider(")
                  .find("CallerCwdContract::Required"),
              std::string::npos);
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

// ANTS-6453 — a comment naming a file outside the root does not get that file
// read: its quotation is reported missing, not confirmed from the outside file.
TEST(CommentClaims, Ants6453FileOutsideRootIsNotRead) {
    QTemporaryDir parent;
    ASSERT_TRUE(parent.isValid());
    writeFile(parent.filePath("out/secret.txt"), "the hidden secret phrase here\n");
    writeFile(parent.filePath("proj/src/a.cpp"),
              "// ../out/secret.txt holds \"the hidden secret phrase here\" as it says.\n");
    const QJsonObject r = CommentClaims::run(
        QFileInfo(parent.filePath("proj")).canonicalFilePath(), {});
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().constData();
    EXPECT_EQ(quotes(rows(r, "findings", QStringLiteral("missing_quotation"))),
              QStringList{QStringLiteral("the hidden secret phrase here")})
        << QJsonDocument(r).toJson().constData();
}
