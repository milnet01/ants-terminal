// Feature-conformance test for ANTS-2222 — read_region symbol-mode returns an
// aggregate's FULL body (struct/class/union), brace-matched, instead of
// stopping at the first member. Drives the pure ReadRegion::extract. See
// spec.md + ROADMAP ANTS-2222 (DOOM_Ants feedback S4).

#include "../../_support/expect.h"
#include "readregion.h"

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

ANTS_TEST_SCOPE();

namespace {
QString joinLines(const QJsonObject &env) {
    QString out;
    for (const auto &v : env.value(QStringLiteral("lines")).toArray())
        out += v.toString() + QLatin1Char('\n');
    return out;
}
QJsonObject extractSym(const QString &path, const char *content, const char *sym) {
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(content);
    f.close();
    ReadRegion::Options opts;
    opts.symbol = QString::fromLatin1(sym);
    return ReadRegion::extract(path, opts);
}
}  // namespace

// INV-1 — a named struct returns its whole body (every field + the closing };),
// not just the declaration line.
TEST(ReadRegionAggregateBody, FullBody) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/r_mesh.h";
    const QJsonObject env = extractSym(path,
        "struct RbVertex {\n"     // 1
        "    float x;\n"          // 2
        "    float y;\n"          // 3
        "    float z;\n"          // 4
        "    int   atlasId;\n"    // 5
        "};\n"                    // 6  <- matching close
        "\n"                      // 7
        "int unrelated() { return 0; }\n",  // 8
        "RbVertex");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("end_line").toInt(), 6);
    const QString body = joinLines(env);
    EXPECT_TRUE(body.contains(QStringLiteral("int   atlasId;")));  // last field present
    EXPECT_TRUE(body.contains(QStringLiteral("};")));              // closing brace present
    EXPECT_FALSE(body.contains(QStringLiteral("unrelated")));      // stops at the struct
}

// INV-2 — a nested method body inside the aggregate doesn't end the range
// early; the close is the aggregate's own matching brace.
TEST(ReadRegionAggregateBody, NestedBracesBalanced) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/widget.h";
    const QJsonObject env = extractSym(path,
        "struct Widget {\n"        // 1
        "    int n;\n"             // 2
        "    int compute() {\n"    // 3
        "        return n + 1;\n"  // 4
        "    }\n"                  // 5  <- inner close, NOT the end
        "};\n"                     // 6  <- aggregate close
        "void after() {}\n",       // 7
        "Widget");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("end_line").toInt(), 6);
    EXPECT_FALSE(joinLines(env).contains(QStringLiteral("after")));
}

// INV-3 — a plain function symbol is unaffected: it returns its own body and
// does not absorb a following declaration.
TEST(ReadRegionAggregateBody, FunctionUnaffected) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/f.cpp";
    const QJsonObject env = extractSym(path,
        "void go() {\n"            // 1
        "    stepOne();\n"         // 2
        "}\n"                      // 3
        "struct Tail { int z; };\n",  // 4
        "go");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_FALSE(joinLines(env).contains(QStringLiteral("struct Tail")));
}
