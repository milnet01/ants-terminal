// Feature-conformance test for ANTS-2228 — file_outline registers
// `typedef struct TAG_s { … } ALIAS_t;` aggregates, keyed by tag + alias, so
// read_region symbol-mode (and ANTS-2222's aggregate body) resolve either.
// Drives the pure FileOutline::compute + ReadRegion::extract. See spec.md.

#include "../../_support/expect.h"
#include "fileoutline.h"
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

// linuxdoom-style C header: tagged + anonymous + nested typedef-structs, plus
// a bare forward decl. Line numbers (1-based):
//  1 typedef struct vertex_s {
//  2     int x;
//  3     int y;
//  4 } vertex_t;
//  5 (blank)
//  6 typedef struct {
//  7     int a;
//  8 } anon_t;
//  9 (blank)
// 10 struct line_s;            <- bare forward decl (unchanged)
// 11 (blank)
// 12 typedef struct sector_s {
// 13     int floor;
// 14     struct { int n; } inner;
// 15 } sector_t;
const char *kHeader =
    "typedef struct vertex_s {\n"       // 1
    "    int x;\n"                       // 2
    "    int y;\n"                       // 3
    "} vertex_t;\n"                      // 4
    "\n"                                 // 5
    "typedef struct {\n"                 // 6
    "    int a;\n"                       // 7
    "} anon_t;\n"                        // 8
    "\n"                                 // 9
    "struct line_s;\n"                   // 10
    "\n"                                 // 11
    "typedef struct sector_s {\n"        // 12
    "    int floor;\n"                   // 13
    "    struct { int n; } inner;\n"     // 14
    "} sector_t;\n";                     // 15

QString writeHeader(const QTemporaryDir &dir) {
    const QString root = QFileInfo(dir.path()).canonicalFilePath();
    const QString path = root + "/r_defs.h";
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(kHeader);
    f.close();
    return path;
}

// Find a symbol by name; returns its {line, kind, signature} or an empty obj.
QJsonObject symByName(const QJsonObject &outline, const char *name) {
    for (const auto &v : outline.value("symbols").toArray()) {
        const QJsonObject o = v.toObject();
        if (o.value("name").toString() == QLatin1String(name)) return o;
    }
    return {};
}

QString bodyOf(const QString &path, const char *sym) {
    ReadRegion::Options opts;
    opts.symbol = QString::fromLatin1(sym);
    const QJsonObject env = ReadRegion::extract(path, opts);
    QString out;
    for (const auto &v : env.value("lines").toArray())
        out += v.toString() + QLatin1Char('\n');
    return out;
}

}  // namespace

// TS-1 — a tagged typedef registers BOTH alias and tag at the opening line.
TEST(FileOutlineTypedefStruct, TaggedRegistersAliasAndTag) {
    QTemporaryDir dir;
    const QString path = writeHeader(dir);
    const QJsonObject outline = FileOutline::compute(
        path, FileOutline::Mode::Cpp, /*includeDoc=*/false, /*maxSymbols=*/100);

    const QJsonObject alias = symByName(outline, "vertex_t");
    const QJsonObject tag   = symByName(outline, "vertex_s");
    ASSERT_FALSE(alias.isEmpty()) << "alias vertex_t not registered";
    ASSERT_FALSE(tag.isEmpty())   << "tag vertex_s not registered";
    EXPECT_EQ(alias.value("line").toInt(), 1);
    EXPECT_EQ(tag.value("line").toInt(), 1);
    EXPECT_EQ(alias.value("kind").toString().toStdString(), "class");
    EXPECT_TRUE(alias.value("signature").toString().startsWith("struct"));
}

// TS-2 — an anonymous typedef registers the alias.
TEST(FileOutlineTypedefStruct, AnonymousRegistersAlias) {
    QTemporaryDir dir;
    const QString path = writeHeader(dir);
    const QJsonObject outline = FileOutline::compute(
        path, FileOutline::Mode::Cpp, false, 100);
    const QJsonObject anon = symByName(outline, "anon_t");
    ASSERT_FALSE(anon.isEmpty()) << "anonymous alias anon_t not registered";
    EXPECT_EQ(anon.value("line").toInt(), 6);
}

// TS-3 — read_region symbol-mode on the alias OR the tag returns the FULL body.
TEST(FileOutlineTypedefStruct, AggregateBodyReadByAliasOrTag) {
    QTemporaryDir dir;
    const QString path = writeHeader(dir);
    for (const char *name : {"vertex_t", "vertex_s"}) {
        const QString body = bodyOf(path, name);
        EXPECT_TRUE(body.contains("typedef struct vertex_s"))
            << name << ": opening line missing";
        EXPECT_TRUE(body.contains("int y;")) << name << ": field missing";
        EXPECT_TRUE(body.contains("} vertex_t;")) << name << ": close missing";
        EXPECT_FALSE(body.contains("anon_t")) << name << ": over-read";
    }
}

// TS-4 — a nested anonymous struct does not end the outer typedef early.
TEST(FileOutlineTypedefStruct, NestedBracesBalanced) {
    QTemporaryDir dir;
    const QString path = writeHeader(dir);
    const QJsonObject outline = FileOutline::compute(
        path, FileOutline::Mode::Cpp, false, 100);
    EXPECT_FALSE(symByName(outline, "sector_t").isEmpty());
    const QString body = bodyOf(path, "sector_t");
    EXPECT_TRUE(body.contains("} inner;"));      // nested member kept
    EXPECT_TRUE(body.contains("} sector_t;"));   // outer close is the end
}

// TS-5 — a bare forward decl still registers; a bodyless typedef adds nothing.
TEST(FileOutlineTypedefStruct, ForwardDeclUnaffected) {
    QTemporaryDir dir;
    const QString path = writeHeader(dir);
    const QJsonObject outline = FileOutline::compute(
        path, FileOutline::Mode::Cpp, false, 100);
    const QJsonObject fwd = symByName(outline, "line_s");
    EXPECT_FALSE(fwd.isEmpty()) << "bare forward decl line_s must still register";
}
