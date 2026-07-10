// ANTS-1636 — feature-conformance test for find_sources. Pure-function
// tests for FindSources::* plus source-grep wiring of the MCP tool.

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"

#include "findsources.h"

#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// Build a tiny synthetic project tree under a QTemporaryDir for the
// findSources() golden-path tests.
struct Sandbox {
    QTemporaryDir dir;
    QString root;
    bool ok = false;

    // QTemporaryDir is non-movable on the project's Qt 6.2 CI baseline
    // (its move ctor only arrived in Qt 6.10), so Sandbox can't be
    // returned by value portably — populate in place instead.
    void build() {
        root = dir.path();
        if (!QDir(root).mkpath(QStringLiteral("src"))) return;
        if (!QDir(root).mkpath(QStringLiteral("tests/features/foo"))) return;
        // src/auditrunner.cpp — filename + body mentions "audit_run".
        write("src/auditrunner.cpp",
              "void AuditRunner::audit_run() {}\n"
              "void AuditRunner::audit_run_loop() {}\n");
        // src/auditrunner.h — header.
        write("src/auditrunner.h",
              "class AuditRunner { void audit_run(); };\n");
        // src/randomthing.cpp — unrelated.
        write("src/randomthing.cpp", "int main() { return 0; }\n");
        // src/testauditengine.cpp — content mentions audit_run.
        write("src/testauditengine.cpp",
              "// engine for test audit. audit_run called here.\n");
        // tests/features/foo/test_foo.cpp — role:test.
        write("tests/features/foo/test_foo.cpp",
              "// audit_run-ish test fixture\n");
        // moc-generated noise that must be skipped.
        write("src/moc_widget.cpp",
              "// audit_run pretend match (skip me)\n");
        ok = true;
    }
    void write(const QString &rel, const QByteArray &body) {
        QFile f(root + QLatin1Char('/') + rel);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
        f.write(body);
        f.close();
    }
};

}  // namespace

TEST(McpFindSources, Tokenise) {
    expect_reset();

    // INV-1 — punctuation split, short-token drop, roadmap-id drop.
    auto toks = FindSources::tokenise(
        QStringLiteral("audit cache invalidation in ANTS-1735 (model auto)"));
    expect(toks.contains(QStringLiteral("audit")),
           "INV-1: tokenises bare words");
    expect(toks.contains(QStringLiteral("cache")), "INV-1: cache token");
    expect(toks.contains(QStringLiteral("invalidation")),
           "INV-1: invalidation token");
    expect(toks.contains(QStringLiteral("model")), "INV-1: model token");
    expect(!toks.contains(QStringLiteral("in")),
           "INV-1: short token (in) dropped");
    expect(!toks.contains(QStringLiteral("ANTS-1735")),
           "INV-1: roadmap id dropped");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpFindSources, VariantsForToken) {
    expect_reset();

    // INV-2 — variants cover snake/camel/dropped-separator.
    auto v = FindSources::variantsForToken(QStringLiteral("audit_run"));
    expect(v.contains(QStringLiteral("audit_run")),
           "INV-2: original lowercase variant present");
    expect(v.contains(QStringLiteral("auditRun")),
           "INV-2: camelCase variant present");
    expect(v.contains(QStringLiteral("auditrun")),
           "INV-2: dropped-separator variant present");
    // PascalCase emitted when token had separators.
    expect(v.contains(QStringLiteral("AuditRun")),
           "INV-2: PascalCase variant present when separators exist");

    // Camel-input → snake variant.
    auto v2 = FindSources::variantsForToken(QStringLiteral("AuditRunner"));
    expect(v2.contains(QStringLiteral("audit_runner")),
           "INV-2: camelCase input → snake_case variant present");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpFindSources, FindSourcesGoldenPath) {
    expect_reset();

    Sandbox s;
    s.build();
    expect(s.ok, "sandbox set up");

    // Query for "audit run" against the synthetic project.
    auto r = FindSources::findSources(
        QStringLiteral("audit run"), s.root);

    expect(!r.files.isEmpty(), "INV-3: golden-path returns matches");

    // Find auditrunner.cpp + auditrunner.h — they should be top hits
    // because the filename matches the tokens.
    bool foundImpl = false, foundHeader = false;
    bool generatedSkipped = true, randomDropped = true;
    int testRoleSeen = 0;
    for (const auto &h : r.files) {
        if (h.path == QStringLiteral("src/auditrunner.cpp")) {
            foundImpl = true;
            expect(h.role == QStringLiteral("impl"),
                   "INV-8: src/*.cpp role = impl");
        }
        if (h.path == QStringLiteral("src/auditrunner.h")) {
            foundHeader = true;
            expect(h.role == QStringLiteral("header"),
                   "INV-8: src/*.h role = header");
        }
        if (h.path.contains(QLatin1String("moc_"))) {
            generatedSkipped = false;
        }
        if (h.path == QStringLiteral("src/randomthing.cpp")) {
            randomDropped = false;
        }
        if (h.role == QStringLiteral("test")) ++testRoleSeen;
    }
    expect(foundImpl,   "INV-3: auditrunner.cpp in results");
    expect(foundHeader, "INV-3: auditrunner.h in results");
    expect(generatedSkipped,
           "INV-6: moc_ generated file skipped");
    expect(randomDropped,
           "INV-7: file with zero hits dropped from results");
    expect(testRoleSeen >= 1,
           "INV-8: tests/ path classified as role=test");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpFindSources, FindSourcesBadRoot) {
    expect_reset();
    auto r = FindSources::findSources(
        QStringLiteral("audit run"),
        QStringLiteral("/nonexistent/path/that/does/not/exist"));
    expect(r.files.isEmpty(),
           "bad root → empty result, no crash");
    EXPECT_EQ(0, expect_failures());
}

TEST(McpFindSources, PrewarmParity) {
    expect_reset();

    Sandbox s;
    s.build();
    expect(s.ok, "sandbox set up");

    // INV-14 — prewarm() shares collectCandidates() with findSources(), so
    // it warms EXACTLY the query's candidate set: its touched-file count
    // equals findSources().filesScanned for the same root.
    const int warmed  = FindSources::prewarm(s.root);
    const int scanned =
        FindSources::findSources(QStringLiteral("audit run"), s.root)
            .filesScanned;
    expect(warmed == scanned,
           "INV-14: prewarm touches the find_sources candidate set");
    expect(warmed > 0, "INV-14: prewarm warms a non-empty tree");

    // INV-14 — bad root warms nothing, no crash (parity with BadRoot).
    expect(FindSources::prewarm(
               QStringLiteral("/nonexistent/path/that/does/not/exist")) == 0,
           "INV-14: prewarm(bad root) == 0");

    EXPECT_EQ(0, expect_failures());
}

// ANTS-3489 — collectCandidates honours .ants/project.json source_roots so a
// C/C++ project laid out beyond src/ + tests/ is actually scanned, instead of
// yielding files_scanned:0 (which reads as "every token unmatched" — an empty
// index indistinguishable from "scanned but no hit").
TEST(McpFindSources, HonoursProjectSettingsRoots) {
    expect_reset();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    // C++ source under a non-default `engine/` root — no src/ or tests/.
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("engine")));
    auto put = [&](const QString &rel, const QByteArray &body) {
        QFile f(root + QLatin1Char('/') + rel);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body);
    };
    put(QStringLiteral("engine/widgetregistry.cpp"),
        "void WidgetRegistry::registerWidget() {}\n");

    // Without a project.json, the src/+tests/ default misses engine/ entirely:
    // files_scanned:0 — the exact ANTS-3489 symptom.
    const auto before =
        FindSources::findSources(QStringLiteral("widget registry"), root);
    expect(before.filesScanned == 0,
           "ANTS-3489: default src/+tests/ walk scans 0 on an engine/ layout");
    expect(before.files.isEmpty(), "ANTS-3489: and returns no files");

    // Declare source_roots:["engine"] — now the walk reaches it.
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral(".ants")));
    put(QStringLiteral(".ants/project.json"),
        "{\"source_roots\": [\"engine\"]}\n");
    const auto after =
        FindSources::findSources(QStringLiteral("widget registry"), root);
    expect(after.filesScanned > 0,
           "ANTS-3489: declared source_roots make the walk scan the tree");
    bool found = false;
    for (const auto &h : after.files)
        if (h.path.contains(QStringLiteral("widgetregistry"))) found = true;
    expect(found, "ANTS-3489: the engine/ source file is now found");

    EXPECT_EQ(0, expect_failures());
}

// ANTS-3489 — a committed virtualenv / node_modules under a flat-root
// source_roots=["."] must not drag vendored C/C++ into the candidate set.
TEST(McpFindSources, PrunesNoiseDirsUnderFlatRoot) {
    expect_reset();

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    auto put = [&](const QString &rel, const QByteArray &body) {
        QFile f(root + QLatin1Char('/') + rel);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body);
    };
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral("node_modules/pkg")));
    ASSERT_TRUE(QDir(root).mkpath(QStringLiteral(".ants")));
    put(QStringLiteral("app.cpp"), "void appMain() { /* widget */ }\n");
    put(QStringLiteral("node_modules/pkg/vendor.h"),
        "// widget widget widget vendored noise\n");
    put(QStringLiteral(".ants/project.json"),
        "{\"source_roots\": [\".\"]}\n");

    const auto r = FindSources::findSources(QStringLiteral("widget"), root);
    bool sawVendor = false;
    for (const auto &h : r.files)
        if (h.path.contains(QStringLiteral("node_modules"))) sawVendor = true;
    expect(!sawVendor, "ANTS-3489: vendored node_modules/ tree is pruned");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpFindSources, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpFile(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    // INV-9 — declaration + definition.
    expect(contains(rcHdr, "cmdFindSources"),
           "INV-9: declared in remotecontrol.h");
    expect(contains(rcCpp, "RemoteControl::cmdFindSources"),
           "INV-9: defined in remotecontrol.cpp");

    // INV-10 — mainwindow registration with Required contract.
    expect(contains(mwCpp, "registerToolProvider(\"find_sources\""),
           "INV-10: find_sources registered in mainwindow.cpp");
    expect(contains(mwCpp,
               "CallerCwdContract::Required,\n"
               "        rcDelegate(&RemoteControl::cmdFindSources)"),
           "INV-10: registered with Required contract");

    // INV-11 — claudeintegration descriptor + cost + bucket + contract.
    expect(contains(ciCpp, "t[\"name\"] = \"find_sources\""),
           "INV-11: tool descriptor present");
    expect(contains(ciCpp, "\"find_sources\""),
           "INV-11: token-cost entry (name appears)");
    expect(contains(ciCpp,
               "name == QLatin1String(\"find_sources\")"),
           "INV-11: kindForName workspace bucket membership");
    // ANTS-2067 — normalise whitespace: the source aligns `return` with
    // spaces, so an exact-string match was fragile to a realignment.
    expect(ants_test::squashWhitespace(ciCpp).find(
               "if (toolName == QStringLiteral(\"find_sources\")) "
               "return C::Required;") != std::string::npos,
           "INV-11: callerCwdContractFor Required branch");

    // ANTS-3415 — `symbol` accepted as an alias for `topic` (handler
    // fallback + schema prop so it isn't flagged in ignored_args).
    expect(contains(rcCpp, "req.value(QStringLiteral(\"symbol\"))"),
           "INV-12: handler reads the `symbol` alias for `topic`");
    expect(contains(ciCpp, "props[\"symbol\"]"),
           "INV-12: schema declares the `symbol` alias prop");

    // INV-13 (ANTS-3435) — an empty result carries a redirect `hint` so a
    // caller doesn't read files_count:0 as a genuine "no such code". The
    // hint names the exact-match verbs (workspace_search / find_definition /
    // find_caller) find_sources' filename/keyword ranking can't substitute for.
    expect(contains(rcCpp, "if (files.isEmpty())") &&
               contains(rcCpp, "out[QStringLiteral(\"hint\")]"),
           "INV-13: cmdFindSources emits a hint on an empty result");
    expect(contains(rcCpp, "ANTS-3435"),
           "INV-13: empty-result hint carries an ANTS-3435 anchor");
    expect(contains(rcCpp, "workspace_search") &&
               contains(rcCpp, "find_caller"),
           "INV-13: hint redirects to workspace_search / find_caller");

    // INV-15 (ANTS-3489) — a DISTINCT hint fires when the candidate set was
    // empty (files_scanned:0), naming the empty-index cause (non-C/C++ project
    // or an undeclared source_root) rather than the generic no-match redirect.
    expect(contains(rcCpp, "res.filesScanned == 0"),
           "INV-15: cmdFindSources branches on an empty candidate set");
    expect(contains(rcCpp, "scanned 0 files") &&
               contains(rcCpp, "source_roots"),
           "INV-15: files_scanned:0 hint names the empty-index cause + source_roots");

    EXPECT_EQ(0, expect_failures());
}
