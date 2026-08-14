// Feature-conformance test for the read_region MCP tool (ANTS-2021).
// Behavioural invariants exercise the pure ReadRegion::extract helper;
// wiring invariants source-scrape the registration sites.
// See spec.md + docs/specs/ANTS-2021.md.

#include "../../_support/expect.h"
#include "readregion.h"

#include <cstdio>
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
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
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
    const std::string rcCpp = ants_test::slurpRemoteControl();
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

// ANTS-3399 / ANTS-3404 — a bare method name resolves an unambiguous
// qualified outline entry via the suffix fallback (`area` → `Box::area`),
// so a caller that knows only the method name need not spell the class.
TEST(McpReadRegion, BareNameQualifiedSuffixMatch) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QStringList src = {
        "struct Box {",                      // 1
        "    int side;",                     // 2
        "};",                                // 3
        "",                                  // 4
        "int Box::area() {",                 // 5  outline: Box::area
        "    return side * side;",           // 6
        "}",                                 // 7
    };
    const QString p = writeFile(dir, "s.cpp", src);
    ReadRegion::Options o; o.symbol = "area";   // bare — no class qualifier
    const QJsonObject env = ReadRegion::extract(p, o);
    ASSERT_TRUE(env.value("ok").toBool())
        << env.value("error").toString().toStdString();
    EXPECT_EQ(env.value("start_line").toInt(), 5);
}

// ANTS-3434 (verification) — read_region symbol-mode via a BARE method name
// on a member whose out-of-line definition carries a TWO-WORD return type
// (`unsigned int Grid::cellCount`). Before ANTS-3433 that member was dropped
// from the outline entirely, so no name — bare or qualified — could resolve
// it and read_region returned symbol_not_found (the ANTS-3434 symptom). With
// ANTS-3433 keeping the outline entry, the ANTS-3399 suffix fallback resolves
// the bare name. This locks the two fixes together end-to-end.
TEST(McpReadRegion, BareNameTwoWordReturnTypeMember) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QStringList src = {
        "struct Grid {",                              // 1
        "    int rows;",                              // 2
        "};",                                         // 3
        "",                                           // 4
        "unsigned int Grid::cellCount() const {",     // 5  outline: Grid::cellCount
        "    return rows * rows;",                     // 6
        "}",                                          // 7
    };
    const QString p = writeFile(dir, "g.cpp", src);
    // Qualified name resolves...
    ReadRegion::Options oq; oq.symbol = "Grid::cellCount";
    const QJsonObject envq = ReadRegion::extract(p, oq);
    ASSERT_TRUE(envq.value("ok").toBool())
        << envq.value("error").toString().toStdString();
    EXPECT_EQ(envq.value("start_line").toInt(), 5);
    // ...and so does the BARE name via the suffix fallback.
    ReadRegion::Options ob; ob.symbol = "cellCount";
    const QJsonObject envb = ReadRegion::extract(p, ob);
    ASSERT_TRUE(envb.value("ok").toBool())
        << envb.value("error").toString().toStdString();
    EXPECT_EQ(envb.value("start_line").toInt(), 5);
}

// ANTS-3513 — the COMPLEMENT of ANTS-3399. When the flat outline indexes only
// the BARE identifier (a namespace free function, `AntsHelper::driftCheck`'s
// outline entry is bare `driftCheck`) but the caller PASTES the qualified name
// (the instinct from find_definition / grep output), the exact pass misses and
// — because the name contains `::` — the bare-suffix fallback is skipped, so
// the pre-fix code returned symbol_not_found. The qualified→bare tail fallback
// resolves it, so both spellings work.
TEST(McpReadRegion, QualifiedNameResolvesBareOutlineEntry) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QStringList src = {
        "namespace ns {",                    // 1  outline: ns (namespace)
        "int display() {",                   // 2  outline: display (bare)
        "    return 7;",                     // 3
        "}",                                 // 4
        "}",                                 // 5
    };
    const QString p = writeFile(dir, "n.cpp", src);
    // Bare name resolves (sanity: the outline entry is bare)...
    ReadRegion::Options ob; ob.symbol = "display";
    const QJsonObject envb = ReadRegion::extract(p, ob);
    ASSERT_TRUE(envb.value("ok").toBool())
        << envb.value("error").toString().toStdString();
    EXPECT_EQ(envb.value("start_line").toInt(), 2);
    // ...and so does the QUALIFIED paste via the tail fallback.
    ReadRegion::Options oq; oq.symbol = "ns::display";
    const QJsonObject envq = ReadRegion::extract(p, oq);
    ASSERT_TRUE(envq.value("ok").toBool())
        << envq.value("error").toString().toStdString();
    EXPECT_EQ(envq.value("start_line").toInt(), 2);
}

// ANTS-3513 — an ambiguous qualified paste (the bare tail matches two symbols)
// must still fall through to symbol_not_found, not silently pick one.
TEST(McpReadRegion, QualifiedNameAmbiguousTailRejected) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QStringList src = {
        "int A::run() { return 1; }",        // 1  outline: A::run
        "int B::run() { return 2; }",        // 2  outline: B::run
    };
    const QString p = writeFile(dir, "s.cpp", src);
    ReadRegion::Options o; o.symbol = "C::run";   // tail `run` matches A::run AND B::run
    const QJsonObject env = ReadRegion::extract(p, o);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "symbol_not_found");
}

// ANTS-3399 / ANTS-3404 — an AMBIGUOUS bare name (two classes share a method
// name) must NOT silently pick one; it falls through to symbol_not_found so
// the caller re-queries with the qualified name.
TEST(McpReadRegion, BareNameAmbiguousSuffixRejected) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QStringList src = {
        "int A::run() { return 1; }",        // 1  outline: A::run
        "int B::run() { return 2; }",        // 2  outline: B::run
    };
    const QString p = writeFile(dir, "s.cpp", src);
    ReadRegion::Options o; o.symbol = "run";    // matches A::run AND B::run
    const QJsonObject env = ReadRegion::extract(p, o);
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "symbol_not_found");
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

// ANTS-4394 — the `~global` sentinel is documented on read_region and
// read_regions.
//
// This one is a DOCUMENTATION defect, not a capability gap, and the
// difference is the whole finding: cmdReadRegion has resolved the sentinel
// through ants::expandGlobalConfigSentinel since it was written, exactly as
// file_outline (ANTS-1390) and doc_integrity (ANTS-3719) do. The schema never
// said so, so a session wanting § 5.4 of a global standard passed an absolute
// ~/.claude path with its PROJECT caller_cwd, got the correct `bad_path`
// refusal, concluded the verb could not reach the tree, and fell back to
// `Bash sed -n '405,430p'` — the raw-tool fallback the SessionStart hook
// exists to avoid, and a line-number guess against a file that shifts as it
// is edited, when `section=` is precisely the right tool.
//
// An undocumented capability is an absent one.
TEST(McpReadRegion, Ants4394GlobalSentinelIsDocumented) {
    const std::string rc = ants_test::slurpRemoteControl();
    const std::string ci =
        ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // The capability: both handlers resolve the sentinel.
    EXPECT_NE(rc.find("expandGlobalConfigSentinel"), std::string::npos)
        << "read_region resolves ~global through the shared helper";

    // The documentation, which is what was missing. Scoped to each verb's own
    // descriptor block so a mention in a sibling cannot satisfy it.
    const auto rrPos  = ci.find("rrTool[\"description\"]");
    const auto rrsPos = ci.find("rrsTool[\"description\"]");
    ASSERT_NE(rrPos, std::string::npos);
    ASSERT_NE(rrsPos, std::string::npos);
    EXPECT_NE(ci.substr(rrPos, 3000).find("~global"), std::string::npos)
        << "read_region's schema must name the sentinel — a capability the "
           "schema does not mention is one callers do not have";
    EXPECT_NE(ci.substr(rrsPos, 3000).find("~global"), std::string::npos)
        << "and read_regions' too, since it shares the resolution path";
}
