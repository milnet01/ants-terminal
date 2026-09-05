// ANTS-1302 — feature-conformance test for focused_test.
// Behavioural coverage of the pure FocusedTest resolution lib +
// source-grep of the cmdFocusedTest / claudeintegration wiring.
// See spec.md and docs/specs/ANTS-1302.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "focusedtest.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

namespace FT = FocusedTest;

namespace {

void writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(body);
}


bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// A valid coverage map with one entry + an ignore for *.md.
FT::CoverageMap mapWith(QHash<QString, QStringList> entries,
                        QStringList def = {}, QStringList ign = {}) {
    FT::CoverageMap m;
    m.valid = true;
    m.schemaVersion = 1;
    m.entries = std::move(entries);
    m.defaultPatterns = std::move(def);
    m.ignoreGlobs = std::move(ign);
    return m;
}

}  // namespace

// INV-1 — empty changed set -> Full.
TEST(McpFocusedTest, ResolveFullOnEmpty) {
    const auto r = FT::resolve({}, mapWith({{"src/foo.cpp", {"Foo"}}}));
    EXPECT_EQ(r.selection, FT::Selection::Full);
}

// INV-2 — mapped file -> Map with the pattern union.
TEST(McpFocusedTest, ResolveMapMode) {
    const auto m = mapWith({{"src/foo.cpp", {"Foo", "Bar"}}});
    const auto r = FT::resolve({"src/foo.cpp"}, m);
    EXPECT_EQ(r.selection, FT::Selection::Map);
    ASSERT_EQ(r.patterns.size(), 2);
    EXPECT_EQ(r.patterns[0], "Foo");
    EXPECT_EQ(r.patterns[1], "Bar");
    ASSERT_EQ(r.mappedFiles.size(), 1);
    EXPECT_EQ(r.mappedFiles[0], "src/foo.cpp");
    EXPECT_TRUE(r.unmappedFiles.isEmpty());
}

// INV-3 — unmapped, non-ignored, no default -> Full.
TEST(McpFocusedTest, ResolveUnmappedForcesFull) {
    const auto m = mapWith({{"src/foo.cpp", {"Foo"}}});
    const auto r = FT::resolve({"src/bar.cpp"}, m);
    EXPECT_EQ(r.selection, FT::Selection::Full);
    ASSERT_EQ(r.unmappedFiles.size(), 1);
    EXPECT_EQ(r.unmappedFiles[0], "src/bar.cpp");
}

// INV-4 — ignored file skipped; mixed with mapped -> Map.
TEST(McpFocusedTest, ResolveIgnoreSkips) {
    const auto m = mapWith({{"src/foo.cpp", {"Foo"}}}, {}, {".md"});
    const auto r = FT::resolve({"src/foo.cpp", "CHANGELOG.md"}, m);
    EXPECT_EQ(r.selection, FT::Selection::Map);
    ASSERT_EQ(r.ignoredFiles.size(), 1);
    EXPECT_EQ(r.ignoredFiles[0], "CHANGELOG.md");
    ASSERT_EQ(r.patterns.size(), 1);
    EXPECT_EQ(r.patterns[0], "Foo");
}

// INV-5 — every changed file ignored -> Full.
TEST(McpFocusedTest, ResolveAllIgnoredFull) {
    const auto m = mapWith({{"src/foo.cpp", {"Foo"}}}, {}, {".md"});
    const auto r = FT::resolve({"README.md", "docs/x.md"}, m);
    EXPECT_EQ(r.selection, FT::Selection::Full);
    EXPECT_EQ(r.ignoredFiles.size(), 2);
    EXPECT_TRUE(r.patterns.isEmpty());
}

// INV-6 — unmapped file + non-empty default -> Map via default.
TEST(McpFocusedTest, ResolveDefaultPatterns) {
    const auto m = mapWith({{"src/foo.cpp", {"Foo"}}}, {"DefSuite"}, {});
    const auto r = FT::resolve({"src/baz.cpp"}, m);
    EXPECT_EQ(r.selection, FT::Selection::Map);
    ASSERT_EQ(r.patterns.size(), 1);
    EXPECT_EQ(r.patterns[0], "DefSuite");
    EXPECT_TRUE(r.unmappedFiles.isEmpty());
}

// INV-7 — loadCoverageMap error/valid classification.
TEST(McpFocusedTest, LoadCoverageMapClassification) {
    {  // absent
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_FALSE(m.valid);
        EXPECT_EQ(m.error, "absent");
    }
    {  // bad_json
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        writeFile(td.path() + "/tests/coverage-map.json", "{ not json ");
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_FALSE(m.valid);
        EXPECT_EQ(m.error, "bad_json");
    }
    {  // bad_schema
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        writeFile(td.path() + "/tests/coverage-map.json",
                  "{\"schema_version\":2,\"map\":{}}");
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_FALSE(m.valid);
        EXPECT_EQ(m.error, "bad_schema");
    }
    {  // ANTS-4460 — a map value that is a STRING, not an array. toArray() on
       // one yields EMPTY, so the key used to be inserted carrying no
       // patterns: the file counted as MAPPED and contributed nothing to the
       // ctest -R regex, and the run selected no tests and reported success.
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        writeFile(td.path() + "/tests/coverage-map.json",
                  "{\"schema_version\":1,\"map\":{\"src/foo.cpp\":\"Foo\"}}");
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_FALSE(m.valid);
        EXPECT_EQ(m.error, "bad_schema");
    }
    {  // the same shape under `default`, which fed the identical empty list.
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        writeFile(td.path() + "/tests/coverage-map.json",
                  "{\"schema_version\":1,\"map\":{},\"default\":\"Foo\"}");
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_FALSE(m.valid);
        EXPECT_EQ(m.error, "bad_schema");
    }
    {  // an ABSENT `default` stays legitimate — the guard must not fire on it.
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        writeFile(td.path() + "/tests/coverage-map.json",
                  "{\"schema_version\":1,\"map\":{\"src/foo.cpp\":[\"Foo\"]}}");
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_TRUE(m.valid);
        EXPECT_TRUE(m.defaultPatterns.isEmpty());
    }
    {  // valid
        QTemporaryDir td; ASSERT_TRUE(td.isValid());
        writeFile(td.path() + "/tests/coverage-map.json",
                  "{\"schema_version\":1,"
                  "\"map\":{\"src/foo.cpp\":[\"Foo\"]},"
                  "\"ignore\":[\".md\"]}");
        const auto m = FT::loadCoverageMap(td.path());
        EXPECT_TRUE(m.valid);
        EXPECT_TRUE(m.error.isEmpty());
        ASSERT_TRUE(m.entries.contains("src/foo.cpp"));
        EXPECT_EQ(m.entries.value("src/foo.cpp"), QStringList{"Foo"});
        EXPECT_EQ(m.ignoreGlobs, QStringList{".md"});
    }
}

// INV-8 — heuristic mode for an invalid map.
TEST(McpFocusedTest, HeuristicMode) {
    FT::CoverageMap invalid;  // valid==false by default
    const auto r = FT::resolve({"src/widget.cpp"}, invalid);
    EXPECT_EQ(r.selection, FT::Selection::Heuristic);
    ASSERT_EQ(r.patterns.size(), 1);
    EXPECT_EQ(r.patterns[0], "widget");

    // Non-source changes alone -> no patterns -> Full.
    const auto r2 = FT::resolve({"notes.md"}, invalid);
    EXPECT_EQ(r2.selection, FT::Selection::Full);
    EXPECT_TRUE(r2.patterns.isEmpty());
    // ANTS-2119 M1 — an unmappable file in heuristic mode is unmappedFiles, not
    // ignoredFiles (heuristic mode consults no ignore globs).
    EXPECT_TRUE(r2.unmappedFiles.contains("notes.md"));
    EXPECT_FALSE(r2.ignoredFiles.contains("notes.md"));
}

// ANTS-2119 M1 — a mappable source file changed ALONGSIDE an unmappable
// non-source file (no basename stem: Makefile, .json, .bin, dotfile) must force
// the full suite, matching map mode's INV-4 conservatism. Previously the
// non-source file was mislabelled as ignoredFiles and the run went Heuristic on
// only the source file's tests — less conservative than map mode.
TEST(McpFocusedTest, Ants2119HeuristicUnmappableForcesFull) {
    FT::CoverageMap invalid;  // valid==false → heuristic mode
    const auto r = FT::resolve({"src/widget.cpp", "data/blob.bin"}, invalid);
    EXPECT_EQ(r.selection, FT::Selection::Full)
        << "an unmappable file must force Full in heuristic mode (err toward "
           "more tests)";
    EXPECT_TRUE(r.unmappedFiles.contains("data/blob.bin"));
    EXPECT_FALSE(r.ignoredFiles.contains("data/blob.bin"))
        << "heuristic mode has no ignore globs — an unmappable file is not "
           "'ignored'";
    // The source file was still recognised (it just doesn't get to run alone).
    EXPECT_TRUE(r.mappedFiles.contains("src/widget.cpp"));
}

// INV-9 — buildCtestRegex OR-joins / handles empty.
TEST(McpFocusedTest, BuildCtestRegex) {
    EXPECT_EQ(FT::buildCtestRegex({"a", "b", "c"}), "(a|b|c)");
    EXPECT_EQ(FT::buildCtestRegex({"Only"}), "(Only)");
    EXPECT_EQ(FT::buildCtestRegex({}), "");
}

// INV-13 — the shipped tests/coverage-map.json is a valid v1 map.
TEST(McpFocusedTest, ShippedCoverageMapValid) {
#ifdef ANTS_SOURCE_DIR
    const auto m = FT::loadCoverageMap(QStringLiteral(ANTS_SOURCE_DIR));
    EXPECT_TRUE(m.valid) << "shipped tests/coverage-map.json failed to "
                            "load: " << m.error.toStdString();
    EXPECT_EQ(m.schemaVersion, 1);
    EXPECT_TRUE(m.entries.contains("src/focusedtest.cpp"));
#else
    GTEST_SKIP() << "ANTS_SOURCE_DIR not defined";
#endif
}

// INV-10..12 — source-grep wiring contract.
TEST(McpFocusedTest, WiringContract) {
#if defined(SRC_RC_HEADER) && defined(ANTS_RC_SOURCES) && \
    defined(SRC_MAINWINDOW_CPP_PATH) && defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ci    = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // INV-10
    EXPECT_TRUE(has(rcHdr, "cmdFocusedTest(const QJsonObject &req)"));
    EXPECT_TRUE(has(rcHdr, "m_focusedTestInFlight"));
    EXPECT_TRUE(has(rcCpp, "RemoteControl::cmdFocusedTest("));
    EXPECT_TRUE(has(rcCpp, "ANTS-1302"));
    EXPECT_TRUE(has(rcCpp, "focused_test_in_flight"));
    EXPECT_TRUE(has(rcCpp, "no_build_dir"));
    EXPECT_TRUE(has(rcCpp, "downgraded_to_full"));
    EXPECT_TRUE(has(rcCpp, "TestResCache::parseCtestOutput"));
    // ANTS-3449 — ctest runs in parallel (-j, capped) so a broad-change
    // full-suite downgrade finishes inside the MCP transport budget instead
    // of surfacing as a spurious -32000 timeout.
    EXPECT_TRUE(has(rcCpp, "ANTS-3449"));
    EXPECT_TRUE(has(rcCpp, "idealThreadCount"));

    // INV-11
    EXPECT_TRUE(has(mwCpp, "registerToolProvider(\"focused_test\""));
    {
        const auto p = mwCpp.find("registerToolProvider(\"focused_test\"");
        ASSERT_NE(p, std::string::npos);
        EXPECT_TRUE(has(mwCpp.substr(p, 180), "CallerCwdContract::Required"));
    }

    // INV-12
    EXPECT_TRUE(has(ci, "t[\"name\"] = \"focused_test\""));
    EXPECT_TRUE(has(ci, "focused_test"));
    EXPECT_TRUE(has(ci, "\"test\""));         // kindForName bucket
    EXPECT_TRUE(has(ci, "800") && has(ci, "4000"));  // kCosts
    EXPECT_TRUE(has(ci, "C::Required"));
    EXPECT_TRUE(has(ci, "R::Expensive"));
    {
        const auto p = ci.find("t[\"name\"] = \"focused_test\"");
        ASSERT_NE(p, std::string::npos);
        const auto end = ci.find("tools.append(t);", p);
        ASSERT_NE(end, std::string::npos);
        const std::string block = ci.substr(p, end - p);
        EXPECT_TRUE(has(block, "selection_hint"));
        EXPECT_TRUE(has(block, "changed_files"));
        EXPECT_TRUE(has(block, "req.append(\"caller_cwd\")"));
    }
#else
    GTEST_SKIP() << "source-path defines not provided";
#endif
}
