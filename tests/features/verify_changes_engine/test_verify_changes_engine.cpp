// Feature-conformance test for ANTS-1289 VerifyEngine.
// Drives VerifyEngine::loadGateConfig + runVerify against synthetic
// project trees under QTemporaryDir. Spec: tests/features/verify_changes_engine/spec.md.

#include "verifyengine.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

VerifyEngine::GateResult *findGate(VerifyEngine::VerifyReport &r,
                                   VerifyEngine::GateName g) {
    for (auto &gr : r.gates) {
        if (gr.name == g) return &gr;
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// INV-1 — hand-rolled .ants/verify.json takes precedence over auto-detect.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv1HandRolledConfigWins) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Set up both: CMakePresets.json (would trigger auto-detect) AND
    // .ants/verify.json (should win).
    writeFile(tmp.path(), "CMakePresets.json", "{}");
    writeFile(tmp.path(), "build/.keep", "");
    writeFile(tmp.path(), "CMakeLists.txt", "project(synth)");
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "true", "format": "plain"}
    })");

    QString src;
    const auto gates = VerifyEngine::loadGateConfig(tmp.path(), &src);
    ASSERT_EQ(gates.size(), 1)
        << "expected hand-rolled config to return exactly 1 gate";
    EXPECT_EQ(gates.first().name, VerifyEngine::GateName::Build);
    EXPECT_EQ(gates.first().command, QStringLiteral("true"));
    EXPECT_EQ(src, QStringLiteral(".ants/verify.json"));
}

// ---------------------------------------------------------------------------
// INV-2 — timeout enforcement.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv2TimeoutKillsHangingGate) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // sleep 30 gets killed after ~1 s once we drop both per-gate and
    // total floors via the test-only injectables on VerifyOptions.
    // Production callers leave the floors at the 10 s default; this
    // test asserts the kill-on-expiry path without paying the 10 s
    // floor on every CI run.
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "sleep 30", "format": "plain"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.timeoutSec         = 1;  // 1 / 1 gate = 1 s per gate
    opts.minPerGateSec      = 1;  // override 10 s floor for the per-gate clamp
    opts.minTotalTimeoutSec = 1;  // override 10 s floor for the total clamp
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *g = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Build);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->ran);
    EXPECT_FALSE(g->passed);
    EXPECT_EQ(g->exitCode, -1);
    EXPECT_TRUE(g->skippedReason.contains(QLatin1String("timeout")))
        << "skippedReason: " << g->skippedReason.toStdString();
    // Wall-clock upper bound only: the lower bound was dropped
    // post-test-audit 2026-05-18 because the timeout semantics are
    // already proven by ran/passed/exitCode/skippedReason above; a
    // fast-runner pass should not flake here.
    EXPECT_LE(g->durationSec, 5.0);
}

// ---------------------------------------------------------------------------
// INV-3 — log capping (line + byte).
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv3LogCappingHonoursLineAndByteCap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // 1000 short lines.
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "for i in $(seq 1 1000); do echo line_$i; done",
                  "format": "plain"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.maxLogLines = 50;
    opts.timeoutSec  = 30;
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *g = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Build);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->ran);
    EXPECT_TRUE(g->passed);
    EXPECT_EQ(g->logTotalLines, 1000);
    EXPECT_TRUE(g->logTruncated);
    // logTail has exactly 50 lines.
    const auto kept = g->logTail.split(QChar('\n'));
    EXPECT_EQ(kept.size(), 50)
        << "expected 50-line log tail; got " << kept.size();
    // First line in tail should be line_951 (1000 - 50 + 1).
    EXPECT_EQ(kept.first(), QStringLiteral("line_951"));
    EXPECT_EQ(kept.last(),  QStringLiteral("line_1000"));
}

TEST(VerifyEngine, Inv3LogCappingByteCapBeatsLineCap) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // 500 lines × 200 chars each = ~100 KiB. With maxLogLines=500 the
    // line cap doesn't bite, so the byte cap (16 KiB) must.
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "for i in $(seq 1 500); do printf '%.0s.' $(seq 1 200); echo; done",
                  "format": "plain"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.maxLogLines = 500;
    opts.timeoutSec  = 30;
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *g = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Build);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->ran);
    EXPECT_EQ(g->logTotalLines, 500);
    EXPECT_TRUE(g->logTruncated);
    const auto bytes = g->logTail.toUtf8().size();
    EXPECT_LE(bytes, 16 * 1024)
        << "logTail exceeded 16 KiB byte cap: " << bytes;
}

// ---------------------------------------------------------------------------
// INV-4 — path-traversal symlink rejection.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv4SymlinkOutsideRootIsRejected) {
    QTemporaryDir tmpRoot;
    QTemporaryDir tmpExternal;
    ASSERT_TRUE(tmpRoot.isValid());
    ASSERT_TRUE(tmpExternal.isValid());

    // External target carrying a bogus config.
    writeFile(tmpExternal.path(), "external.json", R"({
        "build": {"command": "echo poisoned", "format": "plain"}
    })");

    // Project root with a CMakeLists.txt to make auto-detect viable.
    writeFile(tmpRoot.path(), "CMakeLists.txt", "project(synth)");
    writeFile(tmpRoot.path(), "build/.keep", "");

    // Symlink .ants/verify.json -> external.json.
    QDir(tmpRoot.path()).mkpath(QStringLiteral(".ants"));
    const QString linkPath = tmpRoot.path() + QStringLiteral("/.ants/verify.json");
    const QString tgtPath  = tmpExternal.path() + QStringLiteral("/external.json");
    ASSERT_TRUE(QFile::link(tgtPath, linkPath))
        << "failed to create symlink at " << linkPath.toStdString();

    QString src;
    const auto gates = VerifyEngine::loadGateConfig(tmpRoot.path(), &src);
    // Should fall through to auto-detection (CMakeLists+build/ row).
    EXPECT_EQ(src, QStringLiteral("auto-detected"));
    ASSERT_GE(gates.size(), 1);
    EXPECT_EQ(gates.first().command,
              QStringLiteral("cmake --build build"))
        << "expected auto-detect to fire; got: "
        << gates.first().command.toStdString();
}

// ---------------------------------------------------------------------------
// INV-5 — ctest summary regex.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv5CtestParserPopulatesCounts) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // Synthetic ctest-shaped output: 100% pass case.
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "tests": {"command": "printf 'Test #1: foo .... Passed 0.01 sec\nTest #2: bar .... Passed 0.02 sec\n\n100%% tests passed, 0 tests failed out of 2\nTotal Test time (real) = 0.03 sec\n'",
                  "format": "ctest"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.timeoutSec = 30;
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *g = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Tests);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->ran);
    EXPECT_TRUE(g->passed);
    EXPECT_EQ(g->totalCount,  2);
    EXPECT_EQ(g->passedCount, 2);
    EXPECT_TRUE(g->failingTests.isEmpty());
}

TEST(VerifyEngine, Inv5CtestParserExtractsFailingTests) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // 2-failed case with named failures. Use printf escapes so we don't
    // fight quoting. Note %% to escape the percent sign for printf.
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "tests": {"command": "printf 'Test #1: ok .... Passed\nTest #2: bad_one .... Failed\nTest #3: bad_two .... Failed\n\n33%% tests passed, 2 tests failed out of 3\n\nThe following tests FAILED:\n          2 - bad_one (Failed)\n          3 - bad_two (Failed)\n'; exit 8",
                  "format": "ctest"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.timeoutSec = 30;
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *g = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Tests);
    ASSERT_NE(g, nullptr);
    EXPECT_TRUE(g->ran);
    EXPECT_FALSE(g->passed);
    EXPECT_EQ(g->totalCount,  3);
    EXPECT_EQ(g->passedCount, 1);
    ASSERT_EQ(g->failingTests.size(), 2);
    EXPECT_EQ(g->failingTests.at(0), QStringLiteral("bad_one"));
    EXPECT_EQ(g->failingTests.at(1), QStringLiteral("bad_two"));
}

// ---------------------------------------------------------------------------
// INV-6 — tests gate skips when prior build failed.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv6TestsSkipWhenBuildFailed) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "exit 1", "format": "plain"},
        "tests": {"command": "echo SHOULD_NOT_RUN", "format": "plain"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.timeoutSec = 30;
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *b = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Build);
    auto *t = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Tests);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(b->ran);
    EXPECT_FALSE(b->passed);
    EXPECT_FALSE(t->ran);
    EXPECT_EQ(t->skippedReason, QStringLiteral("prior gate failed"));
    EXPECT_EQ(t->logTotalLines, 0);
}

TEST(VerifyEngine, Inv6TestsRunWhenBuildPasses) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), ".ants/verify.json", R"({
        "build": {"command": "true", "format": "plain"},
        "tests": {"command": "echo TESTS_RAN", "format": "plain"}
    })");

    VerifyEngine::VerifyOptions opts;
    opts.timeoutSec = 30;
    const auto rep = VerifyEngine::runVerify(tmp.path(), opts);

    auto *t = findGate(const_cast<VerifyEngine::VerifyReport &>(rep),
                       VerifyEngine::GateName::Tests);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->ran);
    EXPECT_TRUE(t->passed);
    EXPECT_TRUE(t->logTail.contains(QLatin1String("TESTS_RAN")));
}

// ---------------------------------------------------------------------------
// INV-10 (ANTS-3373) — orphaned-source lint. A basename referenced in any
// CMakeLists.txt / *.cmake is not orphaned; one that isn't, is. Build/vendor
// dirs are pruned, so a generated build-tree CMakeLists never masks a real
// orphan.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, Inv10OrphanedSourceLint) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();

    // Top-level list references foo.cpp; a nested *.cmake references baz.cc.
    writeFile(root, "CMakeLists.txt", "add_executable(app foo.cpp)\n");
    writeFile(root, "cmake/extra.cmake", "target_sources(app PRIVATE baz.cc)\n");
    // A pruned build tree references bar.cpp — must NOT count as a reference.
    writeFile(root, "build/CMakeLists.txt", "add_executable(x bar.cpp)\n");

    const QStringList added = {
        QStringLiteral("src/foo.cpp"),   // referenced   → not orphaned
        QStringLiteral("src/bar.cpp"),   // only in build/ (pruned) → orphaned
        QStringLiteral("baz.cc"),        // referenced in .cmake → not orphaned
        QStringLiteral("qux.cpp")};      // nowhere      → orphaned
    const QStringList orphans =
        VerifyEngine::findUnreferencedSources(root, added);
    EXPECT_EQ(orphans, (QStringList{QStringLiteral("src/bar.cpp"),
                                    QStringLiteral("qux.cpp")}));

    // Empty input → empty (fast path, no tree walk).
    EXPECT_TRUE(VerifyEngine::findUnreferencedSources(root, {}).isEmpty());
}

// ---------------------------------------------------------------------------
// Smoke: gateKey() helper maps to the documented strings.
// ---------------------------------------------------------------------------
TEST(VerifyEngine, GateKeyMapsToCanonicalStrings) {
    EXPECT_EQ(VerifyEngine::gateKey(VerifyEngine::GateName::Build),
              QStringLiteral("build"));
    EXPECT_EQ(VerifyEngine::gateKey(VerifyEngine::GateName::Tests),
              QStringLiteral("tests"));
    EXPECT_EQ(VerifyEngine::gateKey(VerifyEngine::GateName::Lint),
              QStringLiteral("lint"));
}
