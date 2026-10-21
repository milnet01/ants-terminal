// ANTS-1302 — feature-conformance test for focused_test.
// Behavioural coverage of the pure FocusedTest resolution lib +
// source-grep of the cmdFocusedTest / claudeintegration wiring.
// See spec.md and docs/specs/ANTS-1302.md.

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "focusedtest.h"
#include "remotecontrol.h"
#include "testrunners.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

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
    defined(ANTS_MAINWINDOW_SOURCES) && defined(SRC_CLAUDE_INTEGRATION_CPP_PATH)
    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string mwCpp = ants_test::slurpMainWindow();
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

// ANTS-5102 — a map pattern is validated for the regex dialect ctest -R
// actually uses (CMake's), not only as PCRE. `\d`, `{n}`, `(?:` and lazy
// quantifiers compile under PCRE but mean something else, or nothing, to
// CMake, so the run silently selected the wrong tests.
TEST(McpFocusedTest, Ants5102PatternsAreCheckedForCMakeRegex) {
    const auto load = [](const char *pattern) {
        QTemporaryDir td;
        writeFile(td.path() + "/tests/coverage-map.json",
                  QStringLiteral(R"({"schema_version":1,"map":{"src/foo.cpp":["%1"]}})")
                      .arg(QString::fromUtf8(pattern)).toUtf8());
        return FT::loadCoverageMap(td.path());
    };
    for (const char *bad : {R"(Foo\\d+)", R"(Foo{2})", R"((?:Foo))", R"(Foo.*?Bar)",
                            R"(\\bFoo)"}) {
        const auto m = load(bad);
        EXPECT_FALSE(m.valid) << bad << " was accepted";
        EXPECT_EQ(m.error, QStringLiteral("bad_pattern")) << bad;
    }
    for (const char *good : {R"(Foo.*Bar)", R"(^(Foo|Bar)\\.)", R"([A-Z]+Test)"}) {
        const auto m = load(good);
        EXPECT_TRUE(m.valid) << good << " was refused: " << m.error.toStdString();
    }
}

// ----- ANTS-5503 — runners other than ctest (INV-9..13 of
// docs/specs/ANTS-5503-multi-runner-tests.md).

namespace {

namespace TR = TestRunners;

TR::Command build(TR::Runner r, const QString &root, const QStringList &changed,
                  const FT::CoverageMap &map = {}) {
    return TR::buildCommand(r, root, changed, map);
}

QStringList sl(std::initializer_list<const char *> xs) {
    QStringList out;
    for (const char *x : xs) out << QString::fromLatin1(x);
    return out;
}

// Puts `dir` alone on PATH for the scope, so a stub stands in for a runner.
struct PathGuard {
    QByteArray saved = qgetenv("PATH");
    explicit PathGuard(const QString &dir) { qputenv("PATH", dir.toLocal8Bit()); }
    ~PathGuard() { qputenv("PATH", saved); }
};

void writeStub(const QString &dir, const char *program, const QByteArray &script) {
    const QString path = dir + QLatin1Char('/') + QString::fromLatin1(program);
    writeFile(path, "#!/bin/sh\n" + script);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QJsonObject callFocused(const QString &root, QJsonObject req) {
    RemoteControl rc(nullptr);
    req[QStringLiteral("caller_cwd")] = root;
    return rc.cmdFocusedTest(req).object();
}

std::string dump(const QJsonObject &o) { return QJsonDocument(o).toJson().toStdString(); }

}  // namespace

// INV-9 — each runner's narrowed and full command, as § 2.4's table says.
TEST(McpFocusedTest, Ants5503Inv9CommandPerRunner) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    const QString root = td.path();
    writeFile(root + "/src/foo.py", "");
    writeFile(root + "/tests/test_foo.py", "");
    writeFile(root + "/Cargo.toml", "[package]\nname = \"top\"\n");
    writeFile(root + "/crates/bee/Cargo.toml", "[package]\nname = \"bee\"\n");

    TR::Command c = build(TR::Runner::Pytest, root, {"src/foo.py"});
    EXPECT_EQ(c.program, "python3");
    EXPECT_EQ(c.args, sl({"-m", "pytest", "-rfE", "./tests/test_foo.py"}));
    EXPECT_EQ(c.fullArgs, sl({"-m", "pytest", "-rfE"}));
    EXPECT_EQ(c.selection, "heuristic");
    EXPECT_EQ(c.selector, sl({"./tests/test_foo.py"}));
    c = build(TR::Runner::Pytest, root, {"tests/test_foo.py"});
    EXPECT_EQ(c.args, sl({"-m", "pytest", "-rfE", "./tests/test_foo.py"}));
    c = build(TR::Runner::Pytest, root, {"src/foo.py", "README.md"});
    EXPECT_EQ(c.selection, "full");
    EXPECT_EQ(c.args, c.fullArgs);
    EXPECT_TRUE(c.selector.isEmpty());

    c = build(TR::Runner::Jest, root, {"src/a.js"});
    EXPECT_EQ(c.program, "npx");
    EXPECT_EQ(c.args, sl({"--no-install", "jest", "--ci", "--findRelatedTests", "./src/a.js"}));
    EXPECT_EQ(c.fullArgs, sl({"--no-install", "jest", "--ci"}));
    EXPECT_EQ(c.selection, "related");

    c = build(TR::Runner::Vitest, root, {"src/a.js"});
    EXPECT_EQ(c.args, sl({"--no-install", "vitest", "related", "--run", "./src/a.js"}));
    EXPECT_EQ(c.fullArgs, sl({"--no-install", "vitest", "run"}));

    c = build(TR::Runner::Cargo, root, {"crates/bee/src/lib.rs", "src/main.rs"});
    EXPECT_EQ(c.program, "cargo");
    EXPECT_EQ(c.args, sl({"test", "--no-fail-fast", "-p", "bee", "-p", "top"}));
    EXPECT_EQ(c.fullArgs, sl({"test", "--no-fail-fast"}));
    EXPECT_EQ(c.selection, "related");

    c = build(TR::Runner::Go, root, {"pkg/a/a.go", "pkg/a/a_test.go", "cmd/b.go"});
    EXPECT_EQ(c.program, "go");
    EXPECT_EQ(c.args, sl({"test", "-v", "./pkg/a", "./cmd"}));
    EXPECT_EQ(c.fullArgs, sl({"test", "-v", "./..."}));
    c = build(TR::Runner::Go, root, {"pkg/a/a.go", "go.mod"});
    EXPECT_EQ(c.selection, "full");
    EXPECT_EQ(c.args, c.fullArgs);
}

// § 2.4 — a cargo file under no package, and a source with no pytest test,
// both run full.
TEST(McpFocusedTest, Ants5503UnplaceableChangeRunsFull) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    writeFile(td.path() + "/Cargo.toml", "[workspace]\nmembers = []\n");
    writeFile(td.path() + "/lonely.py", "");
    EXPECT_EQ(build(TR::Runner::Cargo, td.path(), {"x.rs"}).selection, "full");
    const TR::Command c = build(TR::Runner::Pytest, td.path(), {"lonely.py"});
    EXPECT_EQ(c.selection, "full");
    EXPECT_TRUE(c.unmappedFiles.contains("lonely.py"));
}

// INV-10 — a changed entry reaches the argument list only as ./<path> under
// the root; one outside the root is dropped into ignored_files.
TEST(McpFocusedTest, Ants5503Inv10EntriesAreRootRelativePaths) {
    QTemporaryDir td;
    ASSERT_TRUE(td.isValid());
    for (TR::Runner r : {TR::Runner::Jest, TR::Runner::Pytest}) {
        const TR::Command c =
            build(r, td.path(), {"--config=x.js", "../outside.py", "/etc/passwd"});
        EXPECT_FALSE(c.args.contains("--config=x.js")) << c.args.join(' ').toStdString();
        for (const QString &a : c.args)
            EXPECT_FALSE(a.contains("outside") || a.contains("passwd")) << a.toStdString();
        EXPECT_TRUE(c.ignoredFiles.contains("../outside.py"));
        EXPECT_TRUE(c.ignoredFiles.contains("/etc/passwd"));
    }
    const TR::Command j = build(TR::Runner::Jest, td.path(), {"--config=x.js", "../outside.py"});
    EXPECT_TRUE(j.args.contains("./--config=x.js"));
    // A valid map's ignore globs apply to every runner.
    const TR::Command m = build(TR::Runner::Jest, td.path(), {"README.md", "src/a.js"},
                                mapWith({}, {}, {".md"}));
    EXPECT_TRUE(m.ignoredFiles.contains("README.md"));
    EXPECT_EQ(m.selector, sl({"./src/a.js"}));
}

// INV-11 — a narrowed run reporting no test is re-run full, for jest too.
TEST(McpFocusedTest, Ants5503Inv11ZeroMatchNetEveryRunner) {
    QTemporaryDir td, bin;
    ASSERT_TRUE(td.isValid() && bin.isValid());
    writeFile(td.path() + "/package.json", R"({"devDependencies":{"jest":"^30"}})");
    writeStub(bin.path(), "npx",
              "case \"$*\" in *--findRelatedTests*)\n"
              "  echo 'No tests found, exiting with code 1'; exit 1;;\nesac\n"
              "echo 'Tests:       2 passed, 2 total'\necho 'Time:        0.1 s'\n");
    PathGuard pg(bin.path());
    QJsonObject req;
    req["changed_files"] = QJsonArray{"src/a.js"};
    const QJsonObject out = callFocused(td.path(), req);
    ASSERT_TRUE(out.value("ok").toBool()) << dump(out);
    EXPECT_EQ(out.value("runner").toString(), "jest");
    EXPECT_EQ(out.value("runner_evidence").toString(), "package.json");
    EXPECT_TRUE(out.value("downgraded_to_full").toBool()) << dump(out);
    EXPECT_EQ(out.value("selection").toString(), "full");
    EXPECT_EQ(out.value("passed").toInt(), 2);
    EXPECT_EQ(out.value("nothing_ran"), QJsonValue(false));
    EXPECT_TRUE(out.value("selector").toArray().isEmpty());
    EXPECT_FALSE(out.contains("ctest_filter"));
    EXPECT_FALSE(out.contains("build_dir"));
}

// INV-12 — no marker refuses no_runner; a runner that will not start refuses
// runner_missing; one that crashes refuses runner_failed. ctest keeps its codes.
TEST(McpFocusedTest, Ants5503Inv12Refusals) {
    QTemporaryDir empty, bin;
    ASSERT_TRUE(empty.isValid() && bin.isValid());
    EXPECT_EQ(callFocused(empty.path(), {}).value("code").toString(), "no_runner");

    QJsonObject goReq;
    goReq["runner"] = "go";
    goReq["changed_files"] = QJsonArray{"a.go"};
    {
        PathGuard pg(bin.path());  // holds no go
        const QJsonObject out = callFocused(empty.path(), goReq);
        EXPECT_EQ(out.value("code").toString(), "runner_missing") << dump(out);
    }
    writeStub(bin.path(), "go", "kill -9 $$\n");
    {
        PathGuard pg(bin.path());
        const QJsonObject out = callFocused(empty.path(), goReq);
        EXPECT_EQ(out.value("code").toString(), "runner_failed") << dump(out);
    }
    QJsonObject bad;
    bad["runner"] = "mocha";
    EXPECT_EQ(callFocused(empty.path(), bad).value("code").toString(), "bad_args");

    writeFile(empty.path() + "/CMakeLists.txt", "project(x)\n");
    EXPECT_EQ(callFocused(empty.path(), {}).value("code").toString(), "no_build_dir");
}

// INV-13 — on a ctest project the envelope keeps ANTS-1302's fields and adds
// only runner, runner_evidence, selector and nothing_ran.
TEST(McpFocusedTest, Ants5503Inv13CtestEnvelopeKeys) {
    QTemporaryDir td, bin;
    ASSERT_TRUE(td.isValid() && bin.isValid());
    writeFile(td.path() + "/CMakeLists.txt", "project(x)\n");
    writeFile(td.path() + "/build/CMakeCache.txt", "");
    writeStub(bin.path(), "ctest",
              "echo '1/1 Test #1: foo.Bar ........   Passed    0.01 sec'\n"
              "echo '100% tests passed out of 1'\n");
    PathGuard pg(bin.path());
    QJsonObject req;
    req["changed_files"] = QJsonArray{"src/foo.cpp"};
    const QJsonObject out = callFocused(td.path(), req);
    ASSERT_TRUE(out.value("ok").toBool()) << dump(out);
    const QStringList keys = out.keys();
    const QSet<QString> got(keys.begin(), keys.end());
    const QSet<QString> want{
        // ANTS-1302 § 3.5, with the toJsonWire base it names.
        "ok", "passed", "failed", "skipped", "total", "failing_tests", "selection",
        "ctest_filter", "changed_files", "mapped_files", "unmapped_files",
        "ignored_files", "selection_reason", "duration_ms", "build_dir",
        "exit_code", "schema_version", "recorded_at_ms", "output_byte_size",
        "failing_tests_truncated",
        // ANTS-5503.
        "runner", "runner_evidence", "selector", "nothing_ran"};
    EXPECT_EQ(got, want) << dump(out);
    EXPECT_EQ(out.value("runner").toString(), "ctest");
    EXPECT_EQ(out.value("selector").toArray(), (QJsonArray{"-R", "(foo)"}));
}
