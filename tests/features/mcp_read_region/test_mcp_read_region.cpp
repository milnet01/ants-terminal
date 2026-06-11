// Feature-conformance test for the read_region MCP tool (ANTS-2021).
// Behavioural invariants exercise the pure ReadRegion::extract helper;
// wiring invariants source-scrape the registration sites.
// See spec.md + docs/specs/ANTS-2021.md.

#include "../../_support/expect.h"
#include "readregion.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef ANTS_SOURCE_DIR
#error "ANTS_SOURCE_DIR compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Write `lines` (each gets a trailing \n) to a fresh file; return path.
QString writeFile(const QTemporaryDir &dir, const QString &name,
                  const QStringList &lines) {
    const QString path = dir.path() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(lines.join('\n').toUtf8());
    if (!lines.isEmpty()) f.write("\n");
    f.close();
    return path;
}

QStringList linesOf(const QJsonObject &env) {
    QStringList out;
    for (const auto &v : env.value("lines").toArray()) out << v.toString();
    return out;
}

const QStringList kFixture = {
    "line one",      // 1
    "line two",      // 2
    "line three",    // 3
    "line four",     // 4
    "line five",     // 5
};

ReadRegion::Options lineOpts(int start, int end) {
    ReadRegion::Options o; o.hasLine = true; o.startLine = start; o.endLine = end;
    return o;
}

}  // namespace

// W1 — wiring across the registration sites.
TEST(McpReadRegion, WiringContract) {
    expect_reset();
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string projCpp =
        ants_test::slurpFile(std::string(ANTS_SOURCE_DIR) + "/src/mcpprojection.cpp");

    expect(has(rcHdr, "cmdReadRegion(const QJsonObject &req)"), "W1 decl",
           "remotecontrol.h missing cmdReadRegion declaration");
    expect(has(rcCpp, "cmdReadRegion") && has(rcCpp, "ReadRegion::extract"),
           "W1 impl", "remotecontrol.cpp cmdReadRegion must call ReadRegion::extract");
    expect(has(rcCpp, "validatePath") && has(rcCpp, "read_region"),
           "W1 pathvalidation", "cmdReadRegion must validate the path arg");
    expect(has(ciCpp, "\"read_region\""), "W1 contract+schema",
           "claudeintegration.cpp missing read_region registration");
    expect(has(ciCpp, "callerCwdContractFor") &&
           has(ciCpp, "C::Required") && has(ciCpp, "read_region"),
           "W1 Required", "read_region must be callerCwdContractFor → Required");
    expect(has(ciCpp, "isEtagSupportedTool") && has(ciCpp, "read_region"),
           "W1 etag", "read_region must be in isEtagSupportedTool");

    const char *props[] = {
        "\"start_line\"", "\"end_line\"", "\"symbol\"", "\"max_bytes\"",
    };
    for (const char *p : props) {
        char label[64]; std::snprintf(label, sizeof label, "W1 prop %s", p);
        expect(has(ciCpp, p), label, "read_region schema missing a property");
    }
    expect(has(projCpp, "read_region"), "W1 fields",
           "mcpprojection.cpp isFieldProjectionTool must include read_region");
    expect(has(mwCpp, "registerToolProvider(\"read_region\"") &&
           has(mwCpp, "cmdReadRegion"), "W1 dispatch",
           "mainwindow.cpp must register read_region → cmdReadRegion");
    EXPECT_EQ(0, expect_failures());
}

// B1 — line-range mode: exact slice, EOF clamp, effective echo (INV-1).
TEST(McpReadRegion, LineRange) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeFile(dir, "f.txt", kFixture);
    ASSERT_FALSE(p.isEmpty());

    const QJsonObject env = ReadRegion::extract(p, lineOpts(2, 4));
    EXPECT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("start_line").toInt(), 2);
    EXPECT_EQ(env.value("end_line").toInt(), 4);
    EXPECT_EQ(env.value("returned").toInt(), 3);
    const QStringList got = linesOf(env);
    ASSERT_EQ(got.size(), 3);
    EXPECT_EQ(got.at(0), "line two");
    EXPECT_EQ(got.at(2), "line four");

    // end_line clamps to EOF.
    const QJsonObject clamp = ReadRegion::extract(p, lineOpts(4, 99));
    EXPECT_TRUE(clamp.value("ok").toBool());
    EXPECT_EQ(clamp.value("end_line").toInt(), 5);  // clamped to last line
    EXPECT_EQ(clamp.value("returned").toInt(), 2);
}

// B2 — start past EOF → ok:true, empty (INV-1).
TEST(McpReadRegion, PastEof) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeFile(dir, "f.txt", kFixture);
    const QJsonObject env = ReadRegion::extract(p, lineOpts(10, 12));
    EXPECT_TRUE(env.value("ok").toBool());
    EXPECT_EQ(env.value("returned").toInt(), 0);
    EXPECT_TRUE(env.value("lines").toArray().isEmpty());
}

// B3 — symbol-body mode resolves a qualified member definition (INV-2),
// the realistic "read Class::method" query. file_outline catches
// class/struct/namespace + Class::method forms.
TEST(McpReadRegion, SymbolBody) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QStringList src = {
        "struct Box {",                      // 1  (class outline)
        "    int side;",                     // 2
        "};",                                // 3
        "",                                  // 4
        "int Box::area() {",                 // 5  (Box::area outline)
        "    return side * side;",           // 6
        "}",                                 // 7
    };
    const QString p = writeFile(dir, "s.cpp", src);
    ReadRegion::Options o; o.symbol = "Box::area";
    const QJsonObject env = ReadRegion::extract(p, o);
    ASSERT_TRUE(env.value("ok").toBool()) << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("symbol").toString(), "Box::area");
    EXPECT_EQ(env.value("start_line").toInt(), 5);
    // Box::area is the last outline symbol → body runs to EOF.
    EXPECT_EQ(env.value("end_line").toInt(), 7);
    const QStringList got = linesOf(env);
    ASSERT_FALSE(got.isEmpty());
    EXPECT_TRUE(got.at(0).contains("Box::area"));
}

// B4 — unknown symbol → symbol_not_found (INV-2).
TEST(McpReadRegion, SymbolNotFound) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeFile(dir, "s.cpp", {"int alpha() { return 1; }"});
    ReadRegion::Options o; o.symbol = "nope";
    const QJsonObject env = ReadRegion::extract(p, o);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "symbol_not_found");
}

// B5 — selector exclusivity + range validity (INV-3).
TEST(McpReadRegion, SelectorExclusivity) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeFile(dir, "f.txt", kFixture);

    // Neither selector.
    ReadRegion::Options none;
    EXPECT_EQ(ReadRegion::extract(p, none).value("code").toString(), "bad_args");

    // Both selectors.
    ReadRegion::Options both = lineOpts(1, 2); both.symbol = "x";
    EXPECT_EQ(ReadRegion::extract(p, both).value("code").toString(), "bad_args");

    // start < 1 and end < start.
    EXPECT_EQ(ReadRegion::extract(p, lineOpts(0, 2)).value("code").toString(), "bad_args");
    EXPECT_EQ(ReadRegion::extract(p, lineOpts(4, 2)).value("code").toString(), "bad_args");
}

// B6 — byte cap keeps the head, stops early, clamps over ceiling (INV-8).
TEST(McpReadRegion, ByteCapHead) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeFile(dir, "f.txt", kFixture);

    // Tiny cap → only the head survives, truncated set, end_line < requested.
    ReadRegion::Options o = lineOpts(1, 5);
    o.maxBytes = 12;  // ~one "line one\n" (8 + 3 cost) fits, next would overflow
    const QJsonObject env = ReadRegion::extract(p, o);
    EXPECT_TRUE(env.value("ok").toBool());
    EXPECT_TRUE(env.value("truncated").toBool());
    EXPECT_GE(env.value("returned").toInt(), 1);
    EXPECT_LT(env.value("end_line").toInt(), 5);
    EXPECT_EQ(linesOf(env).at(0), "line one");  // head kept

    // Over-ceiling max_bytes sets bytes_cap_clamped.
    ReadRegion::Options big = lineOpts(1, 5);
    big.maxBytes = ReadRegion::kMaxBytesCeiling + 1;
    const QJsonObject benv = ReadRegion::extract(p, big);
    EXPECT_TRUE(benv.value("bytes_cap_clamped").toBool());
    EXPECT_FALSE(benv.value("truncated").toBool());  // whole small file fits
}
