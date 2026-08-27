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

// ANTS-4735 — brace balance means NOTHING in Python, and applying it there
// returned a SHORT body while asserting truncated:false.
//
// Measured by finbreak against Python's own ast: four of six methods stopped
// partway through their bodies, every cut point on a line carrying a brace,
// while the two methods with no dict literal came back correct. The returned
// slice ended before the function's own os.replace and its except-cleanup, and
// said it was whole.
TEST(ReadRegionAggregateBody, Ants4735PythonBodyNotBraceCapped) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/backup.py";
    const QJsonObject env = extractSym(path,
        "def export_backup(dst):\n"        // 1
        "    manifest = {\n"               // 2
        "        'a': 1,\n"                // 3
        "    }\n"                          // 4  <- the brace scan stopped HERE
        "    tmp = write(manifest)\n"      // 5
        "    os.replace(tmp, dst)\n"       // 6
        "    return dst\n"                 // 7
        "\n"                               // 8
        "def other():\n"                   // 9
        "    return 0\n",                  // 10
        "export_backup");
    ASSERT_TRUE(env.value("ok").toBool());
    const QString body = joinLines(env);
    EXPECT_TRUE(body.contains(QStringLiteral("os.replace(tmp, dst)")))
        << "ANTS-4735: the brace scan closed on the dict literal's `}` and "
           "called that the end of the function, dropping every line after "
           "it -- here the replace, which is the kind of line a reviewer "
           "opens the function to see";
    EXPECT_TRUE(body.contains(QStringLiteral("return dst")));
    EXPECT_FALSE(body.contains(QStringLiteral("def other")))
        << "the extent must still stop at the next symbol";
    EXPECT_GE(env.value("end_line").toInt(), 7);
}

// ANTS-3349 — when a symbol has both a forward declaration and a later
// definition, symbol-mode returns the DEFINITION body, not the 1-line decl
// (DOOM_Ants feedback 2026-06-28; r_vulkan.cpp forward-declares several
// helpers).
TEST(ReadRegionAggregateBody, PrefersDefinitionOverForwardDeclaration) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/r_vk.cpp";
    const QJsonObject env = extractSym(path,
        "void UpdateDescriptor();\n"          // 1  forward declaration
        "\n"                                  // 2
        "void otherThing() { return; }\n"    // 3  single-line body
        "\n"                                  // 4
        "void UpdateDescriptor() {\n"        // 5  definition
        "    doWork();\n"                     // 6
        "    finish();\n"                     // 7
        "}\n",                               // 8
        "UpdateDescriptor");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 5);  // the definition, not line 1
    const QString body = joinLines(env);
    EXPECT_TRUE(body.contains(QStringLiteral("doWork();")));   // definition body
    EXPECT_TRUE(body.contains(QStringLiteral("finish();")));
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

// INV-4 (ANTS-2224) — a function body is capped at its own closing brace even
// when file_outline misses the NEXT symbol. A one-line `extern "C" void …() {`
// is missed by every cpp regex (the `"C"` breaks the return-type group), so the
// outline-derived end for BuildEmitterList would extend to the line before the
// next *recognised* symbol (NextThing), swallowing the extern "C" fn. The brace
// cap stops at BuildEmitterList's own '}'.
TEST(ReadRegionAggregateBody, FunctionOverreadCapped) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/emit.cpp";
    const QJsonObject env = extractSym(path,
        "void BuildEmitterList() {\n"        // 1  <- target symbol (captured)
        "    setupEmitters();\n"             // 2
        "    flushQueue();\n"                // 3
        "}\n"                                // 4  <- real close
        "extern \"C\" void doom_thunk() {\n"  // 5  <- MISSED by file_outline
        "    legacyBridge();\n"              // 6
        "}\n"                                // 7
        "void NextThing() {\n"               // 8  <- next captured symbol
        "    return;\n"                      // 9
        "}\n",                               // 10
        "BuildEmitterList");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("end_line").toInt(), 4)
        << "function body must cap at its own closing brace, not the next "
           "recognised symbol";
    const QString body = joinLines(env);
    EXPECT_TRUE(body.contains(QStringLiteral("flushQueue();")));  // own body kept
    EXPECT_FALSE(body.contains(QStringLiteral("doom_thunk")));    // gap fn excluded
    EXPECT_FALSE(body.contains(QStringLiteral("legacyBridge")));
}

// INV-4 corollary — the LAST function in a file caps at its own close instead of
// reading to EOF (outline end is INT_MAX; the brace cap collapses it).
TEST(ReadRegionAggregateBody, LastFunctionCapsAtBrace) {
    QTemporaryDir dir;
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    QDir().mkpath(root + "/src");
    const QString path = root + "/src/tail.cpp";
    const QJsonObject env = extractSym(path,
        "void only() {\n"   // 1
        "    work();\n"     // 2
        "}\n"               // 3  <- close
        "\n"                // 4  trailing blank lines (would be read to EOF)
        "\n",               // 5
        "only");
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("end_line").toInt(), 3);
}
