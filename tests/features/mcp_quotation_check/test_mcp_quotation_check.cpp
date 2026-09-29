// ANTS-5502 — quotation_check. Contract: docs/specs/ANTS-5502-quotation-check.md;
// the test for each invariant is named in spec.md beside this file.

#include <gtest/gtest.h>

#include "quotationcheckverb.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>

namespace {

const QString kVectors =
    QStringLiteral(ANTS_SRC_DIR "/../tests/features/mcp_quotation_check/vectors");

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QJsonObject item(const QString &path, const QString &text)
{
    return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("text"), text}};
}

QJsonObject call(const QString &root, const QJsonArray &items, const QJsonObject &extra = {})
{
    QJsonObject args = extra;
    args[QStringLiteral("items")] = items;
    return QuotationCheck::run(root, args);
}

QString resultAt(const QJsonObject &env, int i)
{
    return env.value(QStringLiteral("results")).toArray().at(i).toObject()
        .value(QStringLiteral("result")).toString();
}

QJsonObject findingFor(const QJsonObject &env, int index)
{
    for (const QJsonValue &v : env.value(QStringLiteral("findings")).toArray()) {
        if (v.toObject().value(QStringLiteral("index")).toInt(-1) == index)
            return v.toObject();
    }
    return {};
}

// "HIT" / "MISS" / "ERROR" per vector, from expected.tsv.
QList<QPair<QString, QString>> expectations()
{
    QList<QPair<QString, QString>> out;
    const QList<QByteArray> rows = readFile(kVectors + QStringLiteral("/expected.tsv")).split('\n');
    for (const QByteArray &row : rows) {
        const QList<QByteArray> cols = row.split('\t');
        if (cols.size() == 2)
            out.append({QString::fromUtf8(cols[0]), QString::fromUtf8(cols[1])});
    }
    return out;
}

QString scriptVerdict(const QString &result)
{
    if (result == QStringLiteral("hit"))
        return QStringLiteral("HIT");
    if (result == QStringLiteral("miss"))
        return QStringLiteral("MISS");
    if (result == QStringLiteral("not_run"))
        return QStringLiteral("ERROR");
    return QStringLiteral("?") + result;
}

int git(const QString &dir, const QStringList &args)
{
    QProcess p;
    p.setWorkingDirectory(dir);
    p.start(QStringLiteral("git"),
            QStringList{QStringLiteral("-c"), QStringLiteral("user.name=t"),
                        QStringLiteral("-c"), QStringLiteral("user.email=t@t")}
                + args);
    if (!p.waitForFinished(10000))
        return -1;
    return p.exitCode();
}

} // namespace

// INV-1
TEST(QuotationCheck, Inv1VectorsMatchCommittedExpectations)
{
    const auto rows = expectations();
    ASSERT_GE(rows.size(), 14) << "expected.tsv not found under " << kVectors.toStdString();
    for (const auto &[name, want] : rows) {
        const QString quote = QString::fromUtf8(readFile(kVectors + "/" + name + ".quote"));
        const QJsonObject env = call(kVectors, QJsonArray{item(name + ".subject", quote)});
        EXPECT_EQ(scriptVerdict(resultAt(env, 0)), want) << name.toStdString();
    }
}

// INV-1, the script half.
TEST(QuotationCheck, Inv1ScriptAgreesWithExpectations)
{
    const QString script =
        QDir::home().filePath(QStringLiteral(".claude/skills/_shared/quotation-check.sh"));
    if (!QFileInfo::exists(script))
        GTEST_SKIP() << "quotation-check.sh is not installed at " << script.toStdString()
                     << "; the committed expectations were checked without it";
    for (const auto &[name, want] : expectations()) {
        QProcess p;
        p.setWorkingDirectory(kVectors);
        p.start(QStringLiteral("bash"), {script, name + ".subject", name + ".quote"});
        ASSERT_TRUE(p.waitForFinished(10000)) << name.toStdString();
        const int rc = p.exitCode();
        const QString got = rc == 0 ? QStringLiteral("HIT")
                          : rc == 1 ? QStringLiteral("MISS") : QStringLiteral("ERROR");
        EXPECT_EQ(got, want) << name.toStdString();
    }
}

// INV-2
TEST(QuotationCheck, Inv2OneResultPerItemInOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "alpha beta gamma\n"));
    const QJsonObject env = call(dir.path(), QJsonArray{
        item("a.md", "beta"), item("a.md", "delta"), item("gone.md", "x"),
        item("../outside.md", "x")});
    ASSERT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().toStdString();
    const QJsonArray results = env.value("results").toArray();
    ASSERT_EQ(results.size(), 4);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(results.at(i).toObject().value("index").toInt(-1), i);
    EXPECT_EQ(resultAt(env, 0), "hit");
    EXPECT_EQ(resultAt(env, 1), "miss");
    EXPECT_EQ(resultAt(env, 2), "not_run");
    EXPECT_EQ(resultAt(env, 3), "outside_allowed");
    EXPECT_EQ(env.value("items_checked").toInt(), 4);
    const QJsonObject c = env.value("counts").toObject();
    EXPECT_EQ(c.value("hit").toInt() + c.value("miss").toInt() + c.value("not_run").toInt()
                  + c.value("outside_allowed").toInt(),
              4);
    EXPECT_EQ(env.value("normaliser").toString(), QString::fromLatin1(QuotationCheck::kNormaliser));
}

// INV-3
TEST(QuotationCheck, Inv3EachReasonIsNotRun)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "alpha\n"));
    ASSERT_TRUE(writeFile(dir.filePath("locked.md"), "alpha\n"));
    QFile::setPermissions(dir.filePath("locked.md"), QFileDevice::Permissions());
    ASSERT_TRUE(QDir(dir.path()).mkdir("sub"));
    ASSERT_TRUE(writeFile(dir.filePath("big.md"),
                          QByteArray(QuotationCheck::kMaxSubjectBytes + 1, 'x')));

    const QList<QPair<QJsonObject, QString>> cases = {
        {item("a\\b.md", "alpha"), "bad_path"},
        {item("gone.md", "alpha"), "not_found"},
        {item("sub", "alpha"), "not_a_file"},
        {item("locked.md", "alpha"), "unreadable"},
        {item("big.md", "alpha"), "too_large"},
        {item("a.md", QString(QuotationCheck::kMaxTextBytes + 1, 'a')), "text_too_long"},
        {item("a.md", " ** `` "), "empty_text"},
        {QJsonObject{{"path", "a.md"}, {"text", "alpha"}, {"ref", "a b"}}, "bad_ref"},
        {QJsonObject{{"path", "a.md"}, {"text", "alpha"}, {"ref", "HEAD"}}, "ref_unreadable"},
    };
    QJsonArray items;
    for (const auto &c : cases)
        items.append(c.first);
    const QJsonObject env = call(dir.path(), items);
    QFile::setPermissions(dir.filePath("locked.md"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    ASSERT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().toStdString();
    const QJsonArray errors = env.value("check_errors").toArray();
    ASSERT_EQ(errors.size(), cases.size());
    for (int i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(resultAt(env, i), "not_run") << cases[i].second.toStdString();
        const QJsonObject f = findingFor(env, i);
        EXPECT_EQ(f.value("kind").toString(), "not_run") << cases[i].second.toStdString();
        EXPECT_EQ(f.value("reason").toString(), cases[i].second);
        EXPECT_EQ(errors.at(i).toObject().value("reason").toString(), cases[i].second);
    }
}

// INV-4
TEST(QuotationCheck, Inv4MaxBytesNeverTrimsFindings)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "alpha\n"));
    const QJsonObject env = call(dir.path(),
        QJsonArray{item("a.md", "omega"), item("gone.md", "x"), item("../o.md", "x")},
        QJsonObject{{"max_bytes", 1}});
    ASSERT_TRUE(env.value("ok").toBool()) << QJsonDocument(env).toJson().toStdString();
    EXPECT_TRUE(env.value("truncated").toBool());
    EXPECT_LT(env.value("results").toArray().size(), 3);
    EXPECT_EQ(env.value("findings").toArray().size(), 3);
    EXPECT_EQ(env.value("check_errors").toArray().size(), 1);
}

// INV-5
TEST(QuotationCheck, Inv5SymlinkOutsideRootIsOutsideAllowed)
{
    QTemporaryDir dir, other;
    ASSERT_TRUE(dir.isValid() && other.isValid());
    ASSERT_TRUE(writeFile(other.filePath("secret.md"), "the quoted text\n"));
    ASSERT_TRUE(QFile::link(other.filePath("secret.md"), dir.filePath("link.md")));
    const QJsonObject env = call(dir.path(), QJsonArray{item("link.md", "the quoted text")});
    EXPECT_EQ(resultAt(env, 0), "outside_allowed");
}

// INV-5
TEST(QuotationCheck, Inv5AllowedGlobsMatchResolvedPath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath("src/a.md"), "shared words\n"));
    ASSERT_TRUE(writeFile(dir.filePath("docs/x/b.md"), "shared words\n"));
    ASSERT_TRUE(QFile::link(dir.filePath("src/a.md"), dir.filePath("docs/l.md")));
    const QJsonObject env = call(dir.path(),
        QJsonArray{item("src/a.md", "shared words"), item("docs/x/b.md", "shared words"),
                   item("docs/l.md", "shared words")},
        QJsonObject{{"allowed", QJsonArray{"docs/**"}}});
    EXPECT_EQ(resultAt(env, 0), "outside_allowed");
    EXPECT_EQ(resultAt(env, 1), "hit");
    EXPECT_EQ(resultAt(env, 2), "outside_allowed");

    EXPECT_TRUE(QuotationCheck::globMatch("docs/**", "docs/x/b.md"));
    EXPECT_FALSE(QuotationCheck::globMatch("docs/*", "docs/x/b.md"));
    EXPECT_TRUE(QuotationCheck::globMatch("docs/*.md", "docs/b.md"));
    EXPECT_TRUE(QuotationCheck::globMatch("docs/?.md", "docs/b.md"));
    EXPECT_FALSE(QuotationCheck::globMatch("docs/?.md", "docs/bb.md"));
    EXPECT_FALSE(QuotationCheck::globMatch("docs/[b].md", "docs/b.md"));
}

// INV-6
TEST(QuotationCheck, Inv6RefReadsTheBlob)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_EQ(git(dir.path(), {"init", "-q"}), 0);
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "text A alpha\n"));
    ASSERT_EQ(git(dir.path(), {"add", "a.md"}), 0);
    ASSERT_EQ(git(dir.path(), {"commit", "-q", "-m", "A"}), 0);
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "text B beta\n"));
    const QJsonObject env = call(dir.path(), QJsonArray{
        QJsonObject{{"path", "a.md"}, {"text", "text A alpha"}, {"ref", "HEAD"}},
        item("a.md", "text A alpha"),
        QJsonObject{{"path", "a.md"}, {"text", "text A alpha"}, {"ref", "-p"}}});
    EXPECT_EQ(resultAt(env, 0), "hit");
    EXPECT_EQ(resultAt(env, 1), "miss");
    EXPECT_EQ(resultAt(env, 2), "not_run");
    EXPECT_EQ(findingFor(env, 2).value("reason").toString(), "bad_ref");
    EXPECT_EQ(findingFor(env, 0), QJsonObject());
}

// INV-7
TEST(QuotationCheck, Inv7MissPointsAtNearestText)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray line7 = "0123456789abcdefghij and then the tail differs";
    ASSERT_TRUE(writeFile(dir.filePath("a.md"),
                          "one\ntwo\nthree\nfour\nfive\nsix\n" + line7 + "\neight\n"));
    const QJsonObject env =
        call(dir.path(), QJsonArray{item("a.md", "0123456789abcdefghijXYZ not here")});
    EXPECT_EQ(resultAt(env, 0), "miss");
    const QJsonObject f = findingFor(env, 0);
    EXPECT_EQ(f.value("matched_prefix_chars").toInt(), 20);
    EXPECT_EQ(f.value("line").toInt(), 7);
    EXPECT_TRUE(f.value("nearest").toString().startsWith(QString::fromUtf8(line7)))
        << f.value("nearest").toString().toStdString();
}

// INV-8
TEST(QuotationCheck, Inv8SubjectReadOncePerCall)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "alpha beta\n"));
    const QJsonObject env = call(dir.path(),
        QJsonArray{item("a.md", "alpha"), item("a.md", "beta"), item("a.md", "gamma")});
    EXPECT_EQ(env.value("files_read").toInt(), 1);
    EXPECT_EQ(env.value("items_checked").toInt(), 3);
}

// INV-9
TEST(QuotationCheck, Inv9Caps)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath("a.md"), "alpha\n"));
    QJsonArray many;
    for (int i = 0; i <= QuotationCheck::kMaxItems; ++i)
        many.append(item("a.md", "alpha"));
    const QJsonObject over = call(dir.path(), many);
    EXPECT_FALSE(over.value("ok").toBool(true));
    EXPECT_EQ(over.value("code").toString(), "bad_args");

    const QJsonObject env = call(dir.path(), QJsonArray{
        item("a.md", QString(QuotationCheck::kMaxTextBytes + 1, 'a'))});
    EXPECT_EQ(resultAt(env, 0), "not_run");
    EXPECT_EQ(findingFor(env, 0).value("reason").toString(), "text_too_long");

    const QJsonObject none = QuotationCheck::run(dir.path(), QJsonObject{});
    EXPECT_EQ(none.value("code").toString(), "bad_args");
}

// INV-10
TEST(QuotationCheck, Inv10Registration)
{
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
    const auto at = reg.find("registerToolProvider(\"quotation_check\"");
    ASSERT_NE(at, std::string::npos);
    EXPECT_NE(reg.substr(at, 200).find("CallerCwdContract::Required"), std::string::npos);
    const auto scoped = reg.find("registerProjectScopedVerbs");
    ASSERT_NE(scoped, std::string::npos);
    EXPECT_LT(scoped, at);
    EXPECT_NE(ci.find("toolName == QStringLiteral(\"quotation_check\"))"), std::string::npos);
    const auto schema = ci.find("t[\"name\"] = \"quotation_check\";");
    ASSERT_NE(schema, std::string::npos);
    const auto end = ci.find("tools.append(t);", schema);
    ASSERT_NE(end, std::string::npos);
    const std::string body = ci.substr(schema, end - schema);
    EXPECT_NE(body.find("schema[\"type\"] = \"object\";"), std::string::npos);
    EXPECT_NE(body.find("schema[\"additionalProperties\"] = false;"), std::string::npos);
}
