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
    // ANTS-4524 — `fields=` needs no enrolment now; what read_region is still
    // a member of is the compaction table in mcpprojection.cpp.
    expect(has(projCpp, "read_region"), "W1 compact",
           "mcpprojection.cpp's compaction table must include read_region");
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

// ANTS-4700 — the per-line clip. The report's case is a region whose weight
// sits in a few very long lines: a 113-line slice of a hard-wrapped standard
// was 29,244 bytes of which six loop-log rows were 20,446, so a byte cap that
// keeps the head returned neither the structure nor the tail. Clipping per
// line, BEFORE the cap is charged, is what lets the later rows be reached.
TEST(McpReadRegion, Ants4700PerLineClipMakesRoomForMoreLines) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    // Three fat lines then three thin ones. Any head-keeping byte cap large
    // enough to reach the thin rows must otherwise carry the fat ones whole.
    QStringList body;
    for (int i = 0; i < 3; ++i)
        body << QStringLiteral("FAT%1 ").arg(i) + QString(400, QLatin1Char('x'));
    body << QStringLiteral("thin one") << QStringLiteral("thin two")
         << QStringLiteral("thin three");
    const QString p = writeFile(dir, "wide.txt", body);

    // Without the clip the cap is spent on the fat lines.
    ReadRegion::Options plain = lineOpts(1, 6);
    plain.maxBytes = 600;
    const QJsonObject bare = ReadRegion::extract(p, plain);
    ASSERT_TRUE(bare.value("ok").toBool());
    ASSERT_TRUE(bare.value("truncated").toBool())
        << "precondition: the cap must actually bite, or this proves nothing";
    const int bareReturned = bare.value("returned").toInt();
    EXPECT_FALSE(bare.contains("max_line_bytes"))
        << "no clip requested, so no clip keys";
    EXPECT_FALSE(bare.contains("lines_clipped"));

    // With it, the same cap reaches every line.
    ReadRegion::Options clip = lineOpts(1, 6);
    clip.maxBytes     = 600;
    clip.maxLineBytes = 60;
    const QJsonObject env = ReadRegion::extract(p, clip);
    ASSERT_TRUE(env.value("ok").toBool());
    EXPECT_GT(env.value("returned").toInt(), bareReturned)
        << "the clip must make ROOM, not merely shorten what already fitted";
    EXPECT_EQ(env.value("returned").toInt(), 6);
    EXPECT_FALSE(env.value("truncated").toBool());
    EXPECT_EQ(env.value("max_line_bytes").toInt(), 60);
    EXPECT_EQ(env.value("lines_clipped").toInt(), 3)
        << "the three fat lines, and only those";

    // ANTS-4708's own lesson, applied here: hoist once and ASSERT the size
    // before indexing. The EXPECTs above are non-fatal, so on a build where
    // the clip does not fire this ran on to .at(3) of a one-element list and
    // SEGFAULTED the bundle -- which is what it did during this feature's own
    // red check. A crash reports that the binary died rather than which
    // assertion failed, and under a parallel run the surviving output is
    // whichever test happened to be writing.
    const QStringList got = linesOf(env);
    ASSERT_GE(got.size(), 4)
        << "only " << got.size() << " line(s) came back; the assertions below "
           "index into four";
    // The marker is the one workspace_search uses, and the clipped line still
    // carries its identity — the row label a caller recognises.
    const QString first = got.at(0);
    EXPECT_TRUE(first.startsWith(QStringLiteral("FAT0 ")))
        << "got: " << first.toStdString();
    EXPECT_TRUE(first.endsWith(QChar(0x2026)))
        << "the clip marker must be U+2026, as max_match_bytes emits it";
    EXPECT_LE(first.toUtf8().size(), 60);
    // A thin line is untouched — no marker on something that already fitted.
    EXPECT_EQ(got.at(3), "thin one");
}

// ANTS-4700 — the range is the one `max_match_bytes` uses, and an out-of-range
// value is clamped and SAID SO, rather than silently becoming something else.
TEST(McpReadRegion, Ants4700ClampsAndReportsTheLineCap) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const QString p = writeFile(dir, "f.txt", kFixture);

    ReadRegion::Options low = lineOpts(1, 5);
    low.maxLineBytes = 1;
    const QJsonObject lenv = ReadRegion::extract(p, low);
    ASSERT_TRUE(lenv.value("ok").toBool());
    EXPECT_EQ(lenv.value("max_line_bytes").toInt(), ReadRegion::kMinLineBytes);
    EXPECT_TRUE(lenv.value("line_cap_clamped").toBool());
    // Below the floor a clipped line would be all marker and no content, which
    // is why there is a floor rather than an honest 1.
    EXPECT_EQ(lenv.value("lines_clipped").toInt(), 0)
        << "the fixture's lines are short; clamping must not invent clipping";

    ReadRegion::Options high = lineOpts(1, 5);
    high.maxLineBytes = ReadRegion::kMaxLineBytes + 1;
    const QJsonObject henv = ReadRegion::extract(p, high);
    EXPECT_EQ(henv.value("max_line_bytes").toInt(), ReadRegion::kMaxLineBytes);
    EXPECT_TRUE(henv.value("line_cap_clamped").toBool());
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
