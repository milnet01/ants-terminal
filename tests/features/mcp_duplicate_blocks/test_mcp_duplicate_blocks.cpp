// ANTS-5785 — duplicate_blocks. Contract: spec.md beside this file.

#include <gtest/gtest.h>

#include "duplicateblocks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
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

// Six compared lines: the lone `}` is not compared.
const QByteArray kBlock =
    "int total = 0;\n"
    "for (int i = 0; i < n; ++i) {\n"
    "    total += values[i] * weight;\n"
    "    if (total > limit) break;\n"
    "    log(total);\n"
    "}\n"
    "return total;\n";

// kBlock with its fourth compared line edited.
const QByteArray kDrifted =
    "int total = 0;\n"
    "for (int i = 0; i < n; ++i) {\n"
    "    total += values[i] * weight;\n"
    "    if (total >= cap) return -1;\n"
    "    log(total);\n"
    "}\n"
    "return total;\n";

// kBlock with every name changed.
const QByteArray kRenamed =
    "int sum = 0;\n"
    "for (int j = 0; j < count; ++j) {\n"
    "    sum += data[j] * factor;\n"
    "    if (sum > cap) break;\n"
    "    trace(sum);\n"
    "}\n"
    "return sum;\n";

// A five-line block, smaller than kBlock.
const QByteArray kSmall =
    "auto a = open(path);\n"
    "if (!a.ok()) return fail(a);\n"
    "auto b = parse(a.text());\n"
    "store(b);\n"
    "notify(b.size());\n";

// Two unique lines, a comment, then `body`: the body's first line is line 4.
QByteArray wrap(const char *tag, const QByteArray &body) {
    return QByteArray("int ") + tag + "_head();\nint " + tag + "_neck();\n// note\n" + body
         + "int " + tag + "_tail();\n";
}

QTemporaryDir *dirWith(const QMap<QString, QByteArray> &files) {
    auto *dir = new QTemporaryDir;
    for (auto it = files.cbegin(); it != files.cend(); ++it) writeFile(dir->filePath(it.key()), it.value());
    return dir;
}

QJsonObject call(const QTemporaryDir &dir, const QJsonObject &args = {}) {
    return DuplicateBlocks::run(QFileInfo(dir.path()).canonicalFilePath(), args);
}

QJsonArray groups(const QJsonObject &r) { return r.value(QStringLiteral("groups")).toArray(); }

// INV-5 over any reply: no two sites of a group overlap in one file.
bool sitesOverlap(const QJsonObject &r) {
    for (const QJsonValue gv : groups(r)) {
        const QJsonArray sites = gv.toObject().value(QStringLiteral("sites")).toArray();
        for (int i = 0; i < sites.size(); ++i)
            for (int j = i + 1; j < sites.size(); ++j) {
                const QJsonObject a = sites.at(i).toObject(), b = sites.at(j).toObject();
                if (a.value(QStringLiteral("file")) != b.value(QStringLiteral("file"))) continue;
                if (a.value(QStringLiteral("start")).toInt() <= b.value(QStringLiteral("end")).toInt()
                    && b.value(QStringLiteral("start")).toInt() <= a.value(QStringLiteral("end")).toInt())
                    return true;
            }
    }
    return false;
}

} // namespace

TEST(DuplicateBlocks, Inv1ExactCopiesFormAGroup) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", wrap("a", kBlock)},
                                                {"src/b.cpp", wrap("b", kBlock)}}));
    const QJsonObject r = call(*dir);
    ASSERT_TRUE(r.value(QStringLiteral("ok")).toBool()) << QJsonDocument(r).toJson().constData();
    ASSERT_EQ(groups(r).size(), 1) << QJsonDocument(r).toJson().constData();
    const QJsonObject g = groups(r).first().toObject();
    EXPECT_EQ(g.value(QStringLiteral("lines")).toInt(), 6);
    EXPECT_EQ(g.value(QStringLiteral("copies")).toInt(), 2);
    EXPECT_FALSE(g.value(QStringLiteral("diverged")).toBool());
    EXPECT_FALSE(g.value(QStringLiteral("finding_shape")).toBool());
    const QJsonObject site = g.value(QStringLiteral("sites")).toArray().first().toObject();
    EXPECT_EQ(site.value(QStringLiteral("file")).toString(), QStringLiteral("src/a.cpp"));
    EXPECT_EQ(site.value(QStringLiteral("start")).toInt(), 4);
    EXPECT_EQ(site.value(QStringLiteral("end")).toInt(), 10);
}

TEST(DuplicateBlocks, Inv2DivergedCopiesStayOneGroup) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", wrap("a", kBlock)},
                                                {"src/d.cpp", wrap("d", kDrifted)}}));
    const QJsonObject r = call(*dir);
    ASSERT_EQ(groups(r).size(), 1) << QJsonDocument(r).toJson().constData();
    const QJsonObject g = groups(r).first().toObject();
    EXPECT_TRUE(g.value(QStringLiteral("diverged")).toBool());
    EXPECT_TRUE(g.value(QStringLiteral("finding_shape")).toBool());
    EXPECT_EQ(g.value(QStringLiteral("lines")).toInt(), 5);
    QStringList diff;
    for (const QJsonValue v : g.value(QStringLiteral("diff")).toArray())
        diff.append(v.toObject().value(QStringLiteral("text")).toString());
    EXPECT_TRUE(diff.contains(QStringLiteral("if (total >= cap) return -1;")));
    EXPECT_TRUE(diff.contains(QStringLiteral("if (total > limit) break;")));

    EXPECT_TRUE(groups(call(*dir, {{QStringLiteral("similarity"), 0.9}})).isEmpty())
        << "5 of 6 is under 0.9";
}

TEST(DuplicateBlocks, Inv3ThreeCopiesAreOneGroup) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", wrap("a", kBlock)},
                                                {"src/b.cpp", wrap("b", kBlock)},
                                                {"src/c.cpp", wrap("c", kBlock)}}));
    const QJsonObject r = call(*dir);
    ASSERT_EQ(groups(r).size(), 1) << QJsonDocument(r).toJson().constData();
    const QJsonObject g = groups(r).first().toObject();
    EXPECT_EQ(g.value(QStringLiteral("copies")).toInt(), 3);
    EXPECT_TRUE(g.value(QStringLiteral("finding_shape")).toBool());
    EXPECT_FALSE(g.value(QStringLiteral("diverged")).toBool());
}

TEST(DuplicateBlocks, Inv4NormaliseNames) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", wrap("a", kBlock)},
                                                {"src/e.cpp", wrap("e", kRenamed)}}));
    EXPECT_TRUE(groups(call(*dir)).isEmpty());
    const QJsonObject r = call(*dir, {{QStringLiteral("normalise_names"), true}});
    ASSERT_EQ(groups(r).size(), 1) << QJsonDocument(r).toJson().constData();
    EXPECT_TRUE(groups(r).first().toObject().value(QStringLiteral("diverged")).toBool());
}

TEST(DuplicateBlocks, Inv5NoSelfOverlap) {
    QByteArray repeated;
    for (int i = 0; i < 12; ++i) repeated += "x += 1;\n";
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/r.cpp", repeated},
                                                {"src/t.cpp", wrap("t", kBlock) + wrap("u", kBlock)}}));
    const QJsonObject r = call(*dir);
    ASSERT_EQ(groups(r).size(), 1) << QJsonDocument(r).toJson().constData();
    const QJsonArray sites = groups(r).first().toObject().value(QStringLiteral("sites")).toArray();
    ASSERT_EQ(sites.size(), 2);
    EXPECT_EQ(sites.at(0).toObject().value(QStringLiteral("file")).toString(), QStringLiteral("src/t.cpp"));
    EXPECT_LT(sites.at(0).toObject().value(QStringLiteral("end")).toInt(),
              sites.at(1).toObject().value(QStringLiteral("start")).toInt());

    // Nested and staggered copies of one block in one file, the shape that
    // made sites overlap before they were merged.
    const QByteArray nested = kBlock + kSmall + "int gap_one();\n" + kBlock + "int gap_two();\n"
                            + kSmall + "int gap_three();\n" + kBlock + kSmall;
    std::unique_ptr<QTemporaryDir> n(dirWith({{"src/n.cpp", nested}, {"src/m.cpp", kBlock + kSmall}}));
    EXPECT_FALSE(sitesOverlap(call(*n))) << QJsonDocument(call(*n)).toJson().constData();
}

TEST(DuplicateBlocks, Inv6StableIdAndOrder) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", wrap("a", kBlock) + kSmall},
                                                {"src/b.cpp", wrap("b", kBlock)},
                                                {"src/f.cpp", wrap("f", kSmall)}}));
    const QJsonObject r = call(*dir);
    ASSERT_EQ(groups(r).size(), 2) << QJsonDocument(r).toJson().constData();
    EXPECT_EQ(groups(r).at(0).toObject().value(QStringLiteral("lines")).toInt(), 6);
    EXPECT_EQ(groups(r).at(1).toObject().value(QStringLiteral("lines")).toInt(), 5);
    const QString id = groups(r).at(0).toObject().value(QStringLiteral("id")).toString();
    EXPECT_TRUE(QRegularExpression(QStringLiteral("^dup:[0-9a-f]{12}$")).match(id).hasMatch()) << id.toStdString();

    std::unique_ptr<QTemporaryDir> moved(dirWith({{"src/a.cpp", "int moved_down();\n" + wrap("a", kBlock)},
                                                  {"src/b.cpp", wrap("b", kBlock)}}));
    EXPECT_EQ(groups(call(*moved)).first().toObject().value(QStringLiteral("id")).toString(), id);
}

TEST(DuplicateBlocks, Inv7ScopeAndCounts) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", wrap("a", kBlock) + kSmall},
                                                {"src/b.cpp", wrap("b", kBlock)},
                                                {"src/f.cpp", wrap("f", kSmall)},
                                                {"tests/test_x.cpp", wrap("x", kBlock)}}));
    const QJsonObject r = call(*dir, {{QStringLiteral("paths"), QJsonArray{QStringLiteral(".")}}});
    const QJsonObject s = r.value(QStringLiteral("scanned")).toObject();
    EXPECT_EQ(s.value(QStringLiteral("skipped_tests")).toInt(), 1);
    EXPECT_GT(s.value(QStringLiteral("lines_compared")).toInt(), 0);
    EXPECT_EQ(s.value(QStringLiteral("groups_total")).toInt(), 2);
    EXPECT_FALSE(s.value(QStringLiteral("rows_truncated")).toBool());
    EXPECT_EQ(groups(r).first().toObject().value(QStringLiteral("copies")).toInt(), 2);

    const QJsonObject withTests = call(*dir, {{QStringLiteral("paths"), QJsonArray{QStringLiteral(".")}},
                                              {QStringLiteral("include_tests"), true}});
    EXPECT_EQ(groups(withTests).first().toObject().value(QStringLiteral("copies")).toInt(), 3);

    const QJsonObject capped = call(*dir, {{QStringLiteral("top"), 1}});
    EXPECT_EQ(groups(capped).size(), 1);
    EXPECT_TRUE(capped.value(QStringLiteral("scanned")).toObject()
                    .value(QStringLiteral("rows_truncated")).toBool());
}

TEST(DuplicateBlocks, Inv8Refusals) {
    std::unique_ptr<QTemporaryDir> dir(dirWith({{"src/a.cpp", kBlock}}));
    const auto code = [&](const QJsonObject &args) {
        return call(*dir, args).value(QStringLiteral("code")).toString();
    };
    EXPECT_EQ(code({{QStringLiteral("min_lines"), 1}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("min_lines"), 201}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("similarity"), 0.4}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("similarity"), 1.1}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("top"), 0}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{1}}}), QStringLiteral("bad_args"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{QStringLiteral("../outside")}}}),
              QStringLiteral("bad_path"));
    EXPECT_EQ(code({{QStringLiteral("paths"), QJsonArray{QStringLiteral("missing")}}}),
              QStringLiteral("bad_path"));
}

TEST(DuplicateBlocks, Inv9Wiring) {
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
    const auto at = reg.find("registerToolProvider(\"duplicate_blocks\"");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NE(reg.substr(at, 200).find("CallerCwdContract::Required"), std::string::npos);
    const auto scoped = reg.find("registerProjectScopedVerbs");
    ASSERT_NE(scoped, std::string::npos);
    EXPECT_LT(scoped, at);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"duplicate_blocks\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"duplicate_blocks\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    const std::string body = ci.substr(schema, end - schema);
    EXPECT_NE(body.find("schema[\"type\"] = \"object\";"), std::string::npos);
    EXPECT_NE(body.find("schema[\"additionalProperties\"] = false;"), std::string::npos);
}
