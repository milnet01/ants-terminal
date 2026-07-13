// Feature-conformance test for ANTS-1113 DebtSweepEngine pure
// helpers. Drives the engine against synthetic in-memory project
// trees in QTemporaryDir.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "debtsweepengine.h"

namespace {

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    // ANTS-1477 — ADD_FAILURE, not ASSERT_TRUE: this is a void helper
    // called from N TESTs, and ASSERT_* only returns from the *helper*
    // (the calling TEST keeps running on the unwritten file). ADD_FAILURE
    // records a failure attributed to the current TEST so an open failure
    // can't be silently swallowed; we then skip the write of garbage.
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ADD_FAILURE() << "writeFile: cannot open " << qUtf8Printable(abs);
        return;
    }
    f.write(body);
}

QString slurp(const QString &abs) {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

bool gitAvailable() {
    QProcess p;
    p.start(QStringLiteral("git"), {QStringLiteral("--version")});
    return p.waitForStarted(2000) && p.waitForFinished(5000)
           && p.exitCode() == 0;
}

// Run a git command in `dir` with optional extra env (KEY=VALUE).
int runGitIn(const QString &dir, const QStringList &args,
             const QStringList &extraEnv = {}) {
    QProcess p;
    p.setWorkingDirectory(dir);
    if (!extraEnv.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &kv : extraEnv) {
            const int eq = kv.indexOf(QChar('='));
            env.insert(kv.left(eq), kv.mid(eq + 1));
        }
        p.setProcessEnvironment(env);
    }
    p.start(QStringLiteral("git"), args);
    if (!p.waitForStarted(2000)) return -1;
    if (!p.waitForFinished(15000)) { p.kill(); return -1; }
    return p.exitCode();
}

}  // namespace

// ---------------------------------------------------------------------------
// INV-9 / INV-13a — applyMechanicalFix verdicts
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv9ApplyOrphanQUnusedDeletesLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp",
              "int main() {\n"
              "    Q_UNUSED(stale);\n"
              "    return 0;\n"
              "}\n");

    DebtSweepEngine::Finding f;
    f.category    = "code_drift";
    f.detectorId  = "orphan_q_unused";
    f.file        = "src/foo.cpp";
    f.line        = 2;
    f.autoFixable = true;

    const auto v = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_TRUE(v.applied);
    EXPECT_EQ(v.errorCode, QString());
    EXPECT_EQ(v.errorMessage, QString());

    QFile post(tmp.path() + "/src/foo.cpp");
    ASSERT_TRUE(post.open(QIODevice::ReadOnly));
    const QString body = QString::fromUtf8(post.readAll());
    EXPECT_FALSE(body.contains("Q_UNUSED(stale)"));
    EXPECT_EQ(body.split('\n').size(), 4);  // was 5 (4 lines + trailing newline)
}

TEST(DebtSweepEngine, Inv9ApplyNotFixableNoOp) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp", "int main() { return 0; }\n");

    DebtSweepEngine::Finding f;
    f.category    = "code_drift";
    f.detectorId  = "stale_type_comment";  // not in fix table
    f.file        = "src/foo.cpp";
    f.line        = 1;
    f.autoFixable = false;

    const auto v = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_FALSE(v.applied);
    EXPECT_EQ(v.errorCode, QString("not_fixable"));
    EXPECT_FALSE(v.errorMessage.isEmpty());
}

TEST(DebtSweepEngine, Inv13aApplyFileChanged) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp",
              "int main() {\n"
              "    return 0;\n"
              "}\n");

    DebtSweepEngine::Finding f;
    f.category    = "code_drift";
    f.detectorId  = "orphan_q_unused";
    f.file        = "src/foo.cpp";
    f.line        = 2;          // line 2 is `return 0;`, not a marker
    f.autoFixable = true;

    const auto v = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_FALSE(v.applied);
    EXPECT_EQ(v.errorCode, QString("file_changed"));
}

TEST(DebtSweepEngine, Inv9ApplyDetectorMismatch) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp", "int x = 0;\n");

    DebtSweepEngine::Finding f;
    f.category    = "code_drift";
    f.detectorId  = "added_todo";  // claims fixable but no fix table entry
    f.file        = "src/foo.cpp";
    f.line        = 1;
    f.autoFixable = true;

    const auto v = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_FALSE(v.applied);
    EXPECT_EQ(v.errorCode, QString("not_fixable"));
}

// ---------------------------------------------------------------------------
// detectOrphanQUnused — typedef + comment false-positive guards
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, OrphanQUnusedSkipsTypedefsAndComments) {
    if (!gitAvailable()) GTEST_SKIP() << "git not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    ASSERT_EQ(runGitIn(dir, {"init", "-q"}), 0);
    runGitIn(dir, {"config", "user.email", "t@example.com"});
    runGitIn(dir, {"config", "user.name", "Test"});

    // timestamp: a Qt-typedef param (was a typeword-list FP).
    // ghost:     named only inside a // comment (was a self-match FP).
    // realghost: genuinely undeclared → the one true orphan.
    writeFile(dir, "src/x.cpp",
              "void f(qulonglong timestamp) {\n"
              "    // mentions Q_UNUSED(ghost) in prose\n"
              "    Q_UNUSED(timestamp);\n"
              "    Q_UNUSED(realghost);\n"
              "}\n");
    ASSERT_EQ(runGitIn(dir, {"add", "src/x.cpp"}), 0);

    DebtSweepEngine::ScanOptions opt;
    const auto out = DebtSweepEngine::detectOrphanQUnused(dir, opt);
    ASSERT_EQ(out.size(), 1);
    EXPECT_TRUE(out[0].message.contains("realghost"));
}

// ---------------------------------------------------------------------------
// ANTS-3344 — the three code-quality detectors skip test-fixture subtrees
// (tests/features/, tests/audit_fixtures/), which embed bad patterns as
// deliberate conformance inputs, while still flagging real src/ code.
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, CodeQualityDetectorsSkipFixtureData) {
    if (!gitAvailable()) GTEST_SKIP() << "git not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();
    ASSERT_EQ(runGitIn(dir, {"init", "-q"}), 0);
    runGitIn(dir, {"config", "user.email", "t@example.com"});
    runGitIn(dir, {"config", "user.name", "Test"});

    // A file that trips all three detectors: an orphan Q_UNUSED, an
    // obsolete QString idiom, and a statement after `return;`.
    const QByteArray bad =
        "void f() {\n"
        "    Q_UNUSED(ghost);\n"
        "    QString s = QString::null;\n"
        "    return;\n"
        "    trailing();\n"
        "}\n";
    writeFile(dir, "tests/features/foo/test_foo.cpp", bad);
    writeFile(dir, "tests/audit_fixtures/rule/bad.cpp", bad);
    writeFile(dir, "src/real.cpp", bad);  // control — must still flag.
    ASSERT_EQ(runGitIn(dir, {"add", "-A"}), 0);

    DebtSweepEngine::ScanOptions opt;
    for (const auto &out : {DebtSweepEngine::detectOrphanQUnused(dir, opt),
                            DebtSweepEngine::detectObsoleteQStringIdioms(dir, opt),
                            DebtSweepEngine::detectDeadBranchAfterReturn(dir, opt)}) {
        ASSERT_EQ(out.size(), 1);  // only src/real.cpp, never the fixtures.
        EXPECT_EQ(out[0].file, QString("src/real.cpp"));
    }
}

// ---------------------------------------------------------------------------
// INV-4 — detectMissingInvariantTests
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv4MissingInvSurfacesFinding) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "tests/features/foo/spec.md",
              "# Foo feature\n"
              "## Invariants\n"
              "- INV-7. The thing.\n"
              "- INV-8. The other thing.\n");
    writeFile(tmp.path(), "tests/features/foo/test_foo.cpp",
              "// covers INV-7\n"
              "TEST(Foo, T1) {}\n");

    DebtSweepEngine::ScanOptions opt;
    const auto out = DebtSweepEngine::detectMissingInvariantTests(
        tmp.path(), opt);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].category, QString("test_coverage"));
    EXPECT_EQ(out[0].detectorId, QString("missing_inv_test"));
    EXPECT_TRUE(out[0].message.contains("INV-8"));
}

TEST(DebtSweepEngine, Inv4AlphanumericInvIdHandled) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "tests/features/bar/spec.md",
              "# Bar feature\n"
              "- INV-8b. Subordinate.\n");
    writeFile(tmp.path(), "tests/features/bar/test_bar.cpp",
              "// no mention of the alphanumeric INV\n");

    DebtSweepEngine::ScanOptions opt;
    const auto out = DebtSweepEngine::detectMissingInvariantTests(
        tmp.path(), opt);
    ASSERT_EQ(out.size(), 1);
    EXPECT_TRUE(out[0].message.contains("INV-8b"));
}

TEST(DebtSweepEngine, Inv4NoFindingsWhenAllCovered) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "tests/features/baz/spec.md",
              "- INV-1. ok\n- INV-2. ok\n");
    writeFile(tmp.path(), "tests/features/baz/test_baz.cpp",
              "// INV-1 INV-2\n");

    DebtSweepEngine::ScanOptions opt;
    const auto out = DebtSweepEngine::detectMissingInvariantTests(
        tmp.path(), opt);
    EXPECT_EQ(out.size(), 0);
}

TEST(DebtSweepEngine, Inv4MissingFeaturesDirSilentNoOp) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    DebtSweepEngine::ScanOptions opt;
    const auto out = DebtSweepEngine::detectMissingInvariantTests(
        tmp.path(), opt);
    EXPECT_EQ(out.size(), 0);
}

TEST(DebtSweepEngine, Inv4ShellTestCoversInvariant) {
    // A feature tested by a shell script must count as covered (test_*.sh
    // was previously absent from the scan globs — hook_pack FP class).
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "tests/features/sh/spec.md",
              "- INV-1. covered by a shell test\n");
    writeFile(tmp.path(), "tests/features/sh/test_sh.sh",
              "#!/bin/sh\n# asserts INV-1 behaviour\n");

    DebtSweepEngine::ScanOptions opt;
    const auto out = DebtSweepEngine::detectMissingInvariantTests(
        tmp.path(), opt);
    EXPECT_EQ(out.size(), 0);
}

// ---------------------------------------------------------------------------
// INV-10 — templateDebtSweepFoldInBlock
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv10TemplateHeadingAndBullets) {
    DebtSweepEngine::Finding f1;
    f1.category   = "code_drift";
    f1.detectorId = "added_todo";
    f1.file       = "src/foo.cpp";
    f1.line       = 42;
    f1.message    = "TODO: refactor this";

    DebtSweepEngine::Finding f2;
    f2.category   = "doc_drift";
    f2.detectorId = "shipped_without_commit";
    f2.file       = "ROADMAP.md";
    f2.line       = 100;
    f2.message    = "ANTS-9999 is ✅ but no commit mentions it";

    const QString block = DebtSweepEngine::templateDebtSweepFoldInBlock(
        {f1, f2}, {1259, 1260}, "2026-05-13", "ANTS");

    // Heading.
    EXPECT_TRUE(block.startsWith("### 🧹 Debt-sweep fold-in (2026-05-13)\n"));

    // First bullet.
    EXPECT_TRUE(block.contains("- 📋 [ANTS-1259] **TODO: refactor this** at src/foo.cpp:42."));
    EXPECT_TRUE(block.contains("Kind: chore."));
    EXPECT_TRUE(block.contains("Source: debt-sweep-2026-05-13."));

    // Second bullet uses the next ID.
    EXPECT_TRUE(block.contains("[ANTS-1260]"));
}

// ANTS-3497 — the block carries the project's own sniffed prefix, zero-padded
// to the op:append width (mirrors the ANTS-3480 fold-in treatment), NOT a
// hardcoded un-padded `ANTS-<n>`.
TEST(DebtSweepEngine, Inv10ProjectPrefixAndPadding) {
    DebtSweepEngine::Finding f;
    f.category   = "code_drift";
    f.detectorId = "added_todo";
    f.file       = "src/z.cpp";
    f.line       = 3;
    f.message    = "TODO: z";

    const QString block = DebtSweepEngine::templateDebtSweepFoldInBlock(
        {f}, {7}, "2026-07-10", "DOOM");

    EXPECT_TRUE(block.contains("- 📋 [DOOM-0007] **TODO: z** at src/z.cpp:3."))
        << block.toStdString();
    EXPECT_FALSE(block.contains("ANTS"))
        << "the block must not hardcode the ANTS prefix";
    EXPECT_FALSE(block.contains("[DOOM-7]"))
        << "the id must be zero-padded to the op:append min-4 width";
}

TEST(DebtSweepEngine, Inv10DateIsoByteIdentical) {
    DebtSweepEngine::Finding f;
    f.category   = "code_drift";
    f.detectorId = "added_todo";
    f.file       = "src/x.cpp";
    f.line       = 1;
    f.message    = "msg";

    const QString block = DebtSweepEngine::templateDebtSweepFoldInBlock(
        {f}, {1300}, "2026-05-13", "ANTS");

    // Pull the heading date and the bullet's Source: date; require equality.
    QRegularExpression headingRe("### 🧹 Debt-sweep fold-in \\((\\d{4}-\\d{2}-\\d{2})\\)");
    QRegularExpression sourceRe("Source: debt-sweep-(\\d{4}-\\d{2}-\\d{2})");
    const auto h = headingRe.match(block);
    const auto s = sourceRe.match(block);
    ASSERT_TRUE(h.hasMatch());
    ASSERT_TRUE(s.hasMatch());
    EXPECT_EQ(h.captured(1), s.captured(1));
}

TEST(DebtSweepEngine, Inv13bTemplateEmptyOnMismatch) {
    DebtSweepEngine::Finding f;
    f.file = "x";
    EXPECT_EQ(DebtSweepEngine::templateDebtSweepFoldInBlock(
                  {f, f}, {1}, "2026-05-13", "ANTS"),
              QString());
    EXPECT_EQ(DebtSweepEngine::templateDebtSweepFoldInBlock(
                  {}, {}, "2026-05-13", "ANTS"),
              QString());
}

// ---------------------------------------------------------------------------
// INV-11 — triagePrompt
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv11TriagePromptHasOneBlockPerFinding) {
    DebtSweepEngine::Finding a;
    a.category   = "code_drift";
    a.detectorId = "stale_type_comment";
    a.file       = "src/x.cpp";
    a.line       = 12;
    a.message    = "comment references QFakeWidget";

    DebtSweepEngine::Finding b;
    b.category   = "doc_drift";
    b.detectorId = "stale_changelog_bullet";
    b.file       = "CHANGELOG.md";
    b.line       = 5;
    b.message    = "[Unreleased] cites src/missing.cpp";

    const QString p = DebtSweepEngine::triagePrompt({a, b});

    EXPECT_TRUE(p.contains("[code_drift / stale_type_comment] src/x.cpp:12"));
    EXPECT_TRUE(p.contains("[doc_drift / stale_changelog_bullet] CHANGELOG.md:5"));
    EXPECT_TRUE(p.contains("KEEP"));
    EXPECT_TRUE(p.contains("DROP"));
    EXPECT_TRUE(p.contains("DEFER"));
    // The "<= 500 words" footer should land in the prompt.
    EXPECT_TRUE(p.contains("<= 500 words"));
}

// ---------------------------------------------------------------------------
// scanAll honours include-flags (INV-8 from the spec)
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, ScanAllHonoursIncludeFlags) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "tests/features/foo/spec.md",
              "- INV-1. uncovered\n");
    writeFile(tmp.path(), "tests/features/foo/test_foo.cpp", "// nothing\n");

    DebtSweepEngine::ScanOptions opt;
    opt.includeCodeDrift     = false;
    opt.includeDocDrift      = false;
    opt.includePackagingDrift = false;
    // includeTestCoverage stays true.
    const auto out = DebtSweepEngine::scanAll(tmp.path(), opt);
    for (const auto &f : out) {
        EXPECT_NE(f.category, QString("code_drift"));
        EXPECT_NE(f.category, QString("doc_drift"));
        EXPECT_NE(f.category, QString("packaging_drift"));
    }
    bool sawTestCov = false;
    for (const auto &f : out) {
        if (f.category == "test_coverage") sawTestCov = true;
    }
    EXPECT_TRUE(sawTestCov);
}

// ---------------------------------------------------------------------------
// INV-14 / INV-15 — duplicate_include  [ANTS-1358]
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv14DuplicateIncludeFlagsSecond) {
    const QString body =
        "#include \"a.h\"\n"
        "#include \"b.h\"\n"
        "#include \"a.h\"\n"
        "int x;\n";
    const auto out =
        DebtSweepEngine::detail::scanDuplicateIncludes("src/x.cpp", body);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0].detectorId, QString("duplicate_include"));
    EXPECT_EQ(out[0].line, 3);
    EXPECT_TRUE(out[0].autoFixable);

    // Each header once → no finding.
    EXPECT_EQ(DebtSweepEngine::detail::scanDuplicateIncludes(
                  "x", "#include \"a.h\"\n#include \"b.h\"\n").size(), 0);

    // Duplicated across mutually-exclusive #if branches → not flagged.
    const QString cond =
        "#include \"a.h\"\n"
        "#if FOO\n"
        "#include \"a.h\"\n"
        "#endif\n";
    EXPECT_EQ(DebtSweepEngine::detail::scanDuplicateIncludes("x", cond).size(),
              0);
}

TEST(DebtSweepEngine, Inv15ApplyDuplicateIncludeDeletes) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/x.cpp",
              "#include \"a.h\"\n#include \"a.h\"\nint x;\n");

    DebtSweepEngine::Finding f;
    f.detectorId  = "duplicate_include";
    f.file        = "src/x.cpp";
    f.line        = 2;
    f.autoFixable = true;

    const auto v = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_TRUE(v.applied);
    const QString body = slurp(tmp.path() + "/src/x.cpp");
    EXPECT_EQ(body.count("#include \"a.h\""), 1);

    // Re-apply: line 2 is now `int x;` — not a duplicate include.
    const auto v2 = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_FALSE(v2.applied);
    EXPECT_EQ(v2.errorCode, QString("file_changed"));
}

// ---------------------------------------------------------------------------
// INV-16 / INV-17 — obsolete_qstring_idiom  [ANTS-1358]
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv16ObsoleteQStringIdioms) {
    const QString body =
        "QString s = QString::null;\n"
        "x.toAscii();\n"
        "y.fromAscii(z);\n"
        "QString ok = QString();\n";
    const auto out =
        DebtSweepEngine::detail::scanObsoleteQStringIdioms("x", body);
    ASSERT_EQ(out.size(), 3);
    for (const auto &f : out) {
        EXPECT_EQ(f.detectorId, QString("obsolete_qstring_idiom"));
        EXPECT_TRUE(f.autoFixable);
    }
    EXPECT_EQ(DebtSweepEngine::detail::scanObsoleteQStringIdioms(
                  "x", "a.toLatin1(); b = QString();\n").size(), 0);
}

TEST(DebtSweepEngine, Inv17ApplyObsoleteIdiomRewrites) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/x.cpp", "QString s = QString::null;\n");

    DebtSweepEngine::Finding f;
    f.detectorId  = "obsolete_qstring_idiom";
    f.file        = "src/x.cpp";
    f.line        = 1;
    f.autoFixable = true;

    const auto v = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_TRUE(v.applied);
    const QString body = slurp(tmp.path() + "/src/x.cpp");
    EXPECT_TRUE(body.contains("QString s = QString();"));
    EXPECT_FALSE(body.contains("QString::null"));

    // Idempotent: nothing left to rewrite.
    const auto v2 = DebtSweepEngine::applyMechanicalFix(tmp.path(), f);
    EXPECT_FALSE(v2.applied);
    EXPECT_EQ(v2.errorCode, QString("file_changed"));
}

// ---------------------------------------------------------------------------
// INV-18 — dead_branch_after_return  [ANTS-1358]
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv18DeadBranchAfterReturn) {
    const auto dead = DebtSweepEngine::detail::scanDeadBranchAfterReturn(
        "x", "void f() {\n    return;\n    foo();\n}\n");
    ASSERT_EQ(dead.size(), 1);
    EXPECT_EQ(dead[0].detectorId, QString("dead_branch_after_return"));
    EXPECT_EQ(dead[0].line, 3);
    EXPECT_FALSE(dead[0].autoFixable);

    // return immediately before block close → not flagged.
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "void f() {\n    return;\n}\n").size(), 0);

    // conditional return → not flagged.
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "void f() {\n    if (x) return;\n    foo();\n}\n")
                  .size(), 0);

    // break before a switch label → not flagged.
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "switch (x) {\ncase 1:\n    break;\ncase 2:\n    g();\n}\n")
                  .size(), 0);
}

TEST(DebtSweepEngine, Inv18DeadBranchBracelessBodyNotFlagged) {
    // Brace-less `if` body return — the statement after runs when the
    // condition is false, so it is reachable (regression for the 407-FP
    // sweep: `if (cond)\n  return X;\n  next;`).
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "QString f() {\n    if (c)\n        return a();\n"
                       "    return b();\n}\n").size(), 0);

    // Multi-line condition (closing `)` on its own line) — still reachable.
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "QString f() {\n    if (aaa ||\n        bbb)\n"
                       "        return a();\n    return b();\n}\n").size(), 0);

    // Brace-less `else` body return — reachable sibling after.
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "void f() {\n    if (c) { g(); }\n    else\n"
                       "        return;\n    h();\n}\n").size(), 0);

    // Genuine dead code after an *unconditional* return is still flagged.
    EXPECT_EQ(DebtSweepEngine::detail::scanDeadBranchAfterReturn(
                  "x", "void f() {\n    g();\n    return;\n    dead();\n}\n")
                  .size(), 1);
}

// ---------------------------------------------------------------------------
// INV-19 — stale_todo (git-backed)  [ANTS-1358]
// ---------------------------------------------------------------------------

TEST(DebtSweepEngine, Inv19StaleTodoFlagsOldMarker) {
    if (!gitAvailable()) GTEST_SKIP() << "git not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.path();

    ASSERT_EQ(runGitIn(dir, {"init", "-q"}), 0);
    runGitIn(dir, {"config", "user.email", "t@example.com"});
    runGitIn(dir, {"config", "user.name", "Test"});

    writeFile(dir, "src/old.cpp",
              "void f() {\n    // TODO: ancient debt\n    g();\n}\n");
    ASSERT_EQ(runGitIn(dir, {"add", "src/old.cpp"}), 0);
    // Commit dated well over 180 days before today.
    const QStringList dateEnv = {
        "GIT_AUTHOR_DATE=2020-01-01T12:00:00",
        "GIT_COMMITTER_DATE=2020-01-01T12:00:00",
    };
    ASSERT_EQ(runGitIn(dir,
                       {"-c", "commit.gpgsign=false", "commit", "-q",
                        "-m", "old"},
                       dateEnv), 0);

    DebtSweepEngine::ScanOptions opt;
    opt.staleTodoMaxAgeDays = 180;
    const auto out = DebtSweepEngine::detectStaleTodos(dir, opt);
    ASSERT_GE(out.size(), 1);
    bool sawOld = false;
    for (const auto &f : out) {
        if (f.file == "src/old.cpp") {
            sawOld = true;
            EXPECT_EQ(f.detectorId, QString("stale_todo"));
            EXPECT_EQ(f.line, 2);
            EXPECT_FALSE(f.autoFixable);
        }
    }
    EXPECT_TRUE(sawOld);

    // Disabled by threshold <= 0.
    opt.staleTodoMaxAgeDays = 0;
    EXPECT_EQ(DebtSweepEngine::detectStaleTodos(dir, opt).size(), 0);

    // Nothing is that old.
    opt.staleTodoMaxAgeDays = 1000000;
    EXPECT_EQ(DebtSweepEngine::detectStaleTodos(dir, opt).size(), 0);
}

// ---------------------------------------------------------------------------
// ANTS-3346 — evaluateTriageGate (bulk un-triaged defer guard)
// ---------------------------------------------------------------------------

namespace {
QList<DebtSweepEngine::Finding> makeFindings(int count, bool autoFixable) {
    QList<DebtSweepEngine::Finding> out;
    for (int i = 0; i < count; ++i) {
        DebtSweepEngine::Finding f;
        f.category    = "code_drift";
        f.detectorId  = "added_todo";
        f.file        = QStringLiteral("src/f%1.cpp").arg(i);
        f.line        = i + 1;
        f.message     = "m";
        f.autoFixable = autoFixable;
        out.append(f);
    }
    return out;
}
}  // namespace

TEST(DebtSweepEngine, TriageGateAtThresholdAllowed) {
    // Exactly threshold total → allowed (refusal is strictly greater-than).
    const auto batch = makeFindings(
        DebtSweepEngine::kBulkDeferTriageThreshold, /*autoFixable=*/false);
    const auto v = DebtSweepEngine::evaluateTriageGate(batch, /*triaged=*/false);
    EXPECT_TRUE(v.allowed);
    EXPECT_EQ(v.total, DebtSweepEngine::kBulkDeferTriageThreshold);
    EXPECT_EQ(v.threshold, DebtSweepEngine::kBulkDeferTriageThreshold);
    EXPECT_TRUE(v.reason.isEmpty());
}

TEST(DebtSweepEngine, TriageGateBulkRefused) {
    const int n = DebtSweepEngine::kBulkDeferTriageThreshold + 1;
    const auto batch = makeFindings(n, /*autoFixable=*/false);
    const auto v = DebtSweepEngine::evaluateTriageGate(batch, /*triaged=*/false);
    EXPECT_FALSE(v.allowed);
    EXPECT_EQ(v.total, n);
    EXPECT_EQ(v.threshold, DebtSweepEngine::kBulkDeferTriageThreshold);
    EXPECT_FALSE(v.reason.isEmpty());
}

TEST(DebtSweepEngine, TriageGateTriagedOverridesBulk) {
    const int n = DebtSweepEngine::kBulkDeferTriageThreshold + 50;
    const auto batch = makeFindings(n, /*autoFixable=*/false);
    const auto v = DebtSweepEngine::evaluateTriageGate(batch, /*triaged=*/true);
    EXPECT_TRUE(v.allowed);
    EXPECT_TRUE(v.reason.isEmpty());
}

TEST(DebtSweepEngine, TriageGateGatesLargeAutoFixableBatch) {
    // The gate keys on TOTAL bulk, not composition: auto-fixability is not a
    // safety signal (a fixture's deliberate Q_UNUSED is auto-fixable yet a
    // false positive). A big all-auto-fixable scan must still be gated — this
    // is the mostly-[fix] 708-finding "Defer all" case that motivated the fix.
    const int n = DebtSweepEngine::kBulkDeferTriageThreshold + 100;
    const auto batch = makeFindings(n, /*autoFixable=*/true);
    const auto v = DebtSweepEngine::evaluateTriageGate(batch, /*triaged=*/false);
    EXPECT_FALSE(v.allowed);
    EXPECT_EQ(v.total, n);
    EXPECT_EQ(v.nonAutoFixable, 0);   // carried for the message; not the gate
    EXPECT_FALSE(v.reason.isEmpty());
}
