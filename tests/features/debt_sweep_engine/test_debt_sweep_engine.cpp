// Feature-conformance test for ANTS-1113 DebtSweepEngine pure
// helpers. Drives the engine against synthetic in-memory project
// trees in QTemporaryDir.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
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
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
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
        {f1, f2}, {1259, 1260}, "2026-05-13");

    // Heading.
    EXPECT_TRUE(block.startsWith("### 🧹 Debt-sweep fold-in (2026-05-13)\n"));

    // First bullet.
    EXPECT_TRUE(block.contains("- 📋 [ANTS-1259] **TODO: refactor this** at src/foo.cpp:42."));
    EXPECT_TRUE(block.contains("Kind: chore."));
    EXPECT_TRUE(block.contains("Source: debt-sweep-2026-05-13."));

    // Second bullet uses the next ID.
    EXPECT_TRUE(block.contains("[ANTS-1260]"));
}

TEST(DebtSweepEngine, Inv10DateIsoByteIdentical) {
    DebtSweepEngine::Finding f;
    f.category   = "code_drift";
    f.detectorId = "added_todo";
    f.file       = "src/x.cpp";
    f.line       = 1;
    f.message    = "msg";

    const QString block = DebtSweepEngine::templateDebtSweepFoldInBlock(
        {f}, {1300}, "2026-05-13");

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
                  {f, f}, {1}, "2026-05-13"),
              QString());
    EXPECT_EQ(DebtSweepEngine::templateDebtSweepFoldInBlock(
                  {}, {}, "2026-05-13"),
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
