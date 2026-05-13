// Feature-conformance test for ANTS-1112 IndieReviewEngine pure
// helpers. Drives the engine against synthetic in-memory CLAUDE.md
// + a tmpdir src/ tree.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

#include "indiereviewengine.h"
#include "subsystemmap.h"

namespace {

void writeFile(const QString &dir, const QString &rel,
               const QByteArray &body) {
    const QString abs = QDir(dir).filePath(rel);
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(body);
}

QString seedProject(QTemporaryDir &tmp,
                    const QByteArray &claudeMd,
                    const QStringList &srcFiles) {
    writeFile(tmp.path(), "CLAUDE.md", claudeMd);
    for (const QString &name : srcFiles) {
        writeFile(tmp.path(), QStringLiteral("src/") + name,
                  QByteArray("// stub for ") + name.toUtf8());
    }
    SubsystemMap::clearCacheForTests();
    return tmp.path();
}

}  // namespace

TEST(IndieReviewEngine, Inv1DerivePartitionFromClaudeMd) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QByteArray claudeMd =
        "# Test Project\n\n"
        "## Module map (src/)\n\n"
        "- `foo` \xe2\x80\x94 does foo things\n"
        "- `bar` \xe2\x80\x94 does bar things\n"
        "## Other section\n";
    seedProject(tmp, claudeMd,
                QStringList{"foo.h", "foo.cpp", "bar.h", "bar.cpp"});

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    ASSERT_GE(lanes.size(), 2);

    bool sawFoo = false, sawBar = false;
    for (const auto &l : lanes) {
        if (l.name == "foo") {
            sawFoo = true;
            EXPECT_TRUE(l.sourcePaths.contains("src/foo.h"));
            EXPECT_TRUE(l.sourcePaths.contains("src/foo.cpp"));
        }
        if (l.name == "bar") sawBar = true;
    }
    EXPECT_TRUE(sawFoo);
    EXPECT_TRUE(sawBar);
}

TEST(IndieReviewEngine, Inv2DerivePartitionHonoursOverride) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seedProject(tmp,
                "# x\n## Module map (src/)\n- `ignored` \xe2\x80\x94 wrong\n",
                QStringList{"ignored.h", "ignored.cpp"});

    writeFile(tmp.path(), ".indie-review/partition.json",
              "{\"version\":1,\"lanes\":[{"
              "\"name\":\"override-lane\","
              "\"summary\":\"from override\","
              "\"sourcePaths\":[\"src/ignored.h\"]}]}");

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    ASSERT_EQ(lanes.size(), 1);
    EXPECT_EQ(lanes[0].name, "override-lane");
    EXPECT_EQ(lanes[0].summary, "from override");
    EXPECT_TRUE(lanes[0].sourcePaths.contains("src/ignored.h"));
}

TEST(IndieReviewEngine, Inv3AssembleBriefShape) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    seedProject(tmp,
                "# x\n## Module map (src/)\n- `foo` \xe2\x80\x94 F\n",
                QStringList{"foo.h", "foo.cpp"});
    // Empty ROADMAP just to exercise the slice block.
    writeFile(tmp.path(), "ROADMAP.md", "# Roadmap\n");

    const auto lanes = IndieReviewEngine::derivePartition(tmp.path());
    ASSERT_FALSE(lanes.isEmpty());

    const QString brief = IndieReviewEngine::assembleBrief(tmp.path(), lanes[0]);
    EXPECT_TRUE(brief.startsWith("=== Lane: foo ==="));
    EXPECT_TRUE(brief.contains("=== file: src/foo.h ==="));
    EXPECT_TRUE(brief.contains("=== file: src/foo.cpp ==="));
    EXPECT_TRUE(brief.contains("=== ROADMAP slice ==="));
    EXPECT_TRUE(brief.contains("docs/standards/coding.md"));
    EXPECT_TRUE(brief.contains("docs/standards/testing.md"));
    EXPECT_TRUE(brief.contains("docs/standards/documentation.md"));
}

TEST(IndieReviewEngine, Inv4ExtractCitationsLineLevel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp", "// stub\n");

    const QString report =
        "I noticed an issue at src/foo.cpp:42 — the function is wrong.\n"
        "Also see fabricated/path.cpp:99 which doesn't exist.\n";
    const auto cites = IndieReviewEngine::extractFileLineCitations(
        tmp.path(), report);
    bool sawFooLine = false;
    bool sawFabricated = false;
    for (const auto &c : cites) {
        if (c.file == "src/foo.cpp" && c.line == 42) sawFooLine = true;
        if (c.file.contains("fabricated")) sawFabricated = true;
    }
    EXPECT_TRUE(sawFooLine);
    EXPECT_FALSE(sawFabricated);  // dropped on resolve
}

TEST(IndieReviewEngine, ExtractCitationsEmptyReport) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_TRUE(IndieReviewEngine::extractFileLineCitations(
        tmp.path(), QString()).isEmpty());
}

TEST(IndieReviewEngine, Inv5CorroboratedTwoLanesSameLine) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp", "// stub\n");

    QHash<QString, QString> reports;
    reports["lane-a"] = "Bug at src/foo.cpp:42 here.";
    reports["lane-b"] = "Same site src/foo.cpp:42 — also broken.";

    const auto found = IndieReviewEngine::corroboratedFindings(
        tmp.path(), reports, /*minLanes=*/2);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].file, "src/foo.cpp");
    EXPECT_EQ(found[0].line, 42);
    EXPECT_EQ(found[0].citingLanes.size(), 2);
    EXPECT_TRUE(found[0].citingLanes.contains("lane-a"));
    EXPECT_TRUE(found[0].citingLanes.contains("lane-b"));
}

TEST(IndieReviewEngine, Inv5SingleLaneNotCorroborated) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp", "// stub\n");

    QHash<QString, QString> reports;
    reports["lane-a"] = "Only here: src/foo.cpp:42.";

    const auto found = IndieReviewEngine::corroboratedFindings(
        tmp.path(), reports, /*minLanes=*/2);
    EXPECT_TRUE(found.isEmpty());
}

TEST(IndieReviewEngine, Inv6FileLevelDistinctFromLineLevel) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "src/foo.cpp", "// stub\n");

    QHash<QString, QString> reports;
    reports["lane-a"] = "See src/foo.cpp (file-level concern).";
    reports["lane-b"] = "And src/foo.cpp:42 (line-level).";

    const auto found = IndieReviewEngine::corroboratedFindings(
        tmp.path(), reports, /*minLanes=*/2);
    EXPECT_TRUE(found.isEmpty());  // distinct keys, neither has 2 lanes

    const auto found1 = IndieReviewEngine::corroboratedFindings(
        tmp.path(), reports, /*minLanes=*/1);
    EXPECT_EQ(found1.size(), 2);  // each emitted independently
}

TEST(IndieReviewEngine, Inv7SynthesisPromptListsAllLanes) {
    QHash<QString, QString> reports;
    reports["alpha"] = "Report A.";
    reports["beta"]  = "Report B.";
    reports["gamma"] = "Report C.";
    const QString prompt = IndieReviewEngine::synthesisPrompt(reports, "");
    EXPECT_TRUE(prompt.contains("## Lane: alpha"));
    EXPECT_TRUE(prompt.contains("## Lane: beta"));
    EXPECT_TRUE(prompt.contains("## Lane: gamma"));
    EXPECT_TRUE(prompt.contains("=== Threat model extras ==="));
    EXPECT_TRUE(prompt.contains("(none provided)"));
}

TEST(IndieReviewEngine, Inv8TemplateFoldInShape) {
    IndieReviewEngine::CorroboratedFinding f;
    f.file = "src/foo.cpp";
    f.line = 42;
    f.citingLanes = {"lane-a", "lane-b"};
    f.contexts = {"ctx-a", "ctx-b"};
    QList<IndieReviewEngine::CorroboratedFinding> findings = {f};
    QList<int> ids = {1500};
    const QString out = IndieReviewEngine::templateIndieReviewFoldInBlock(
        findings, ids, "2026-05-13");
    EXPECT_TRUE(out.startsWith("### 🔍 Indie-review fold-in (2026-05-13)"));
    EXPECT_TRUE(out.contains("- 📋 [ANTS-1500]"));
    EXPECT_TRUE(out.contains("Kind: review-fix."));
    EXPECT_TRUE(out.contains("Source: indie-review-2026-05-13."));
    EXPECT_TRUE(out.contains("`src/foo.cpp:42`"));
}

TEST(IndieReviewEngine, TemplateEmptyOnSizeMismatch) {
    IndieReviewEngine::CorroboratedFinding empty;
    QList<IndieReviewEngine::CorroboratedFinding> findings;
    findings.append(empty);
    findings.append(empty);
    QList<int> ids = {1};
    EXPECT_TRUE(IndieReviewEngine::templateIndieReviewFoldInBlock(
        findings, ids, "2026-05-13").isEmpty());
}

TEST(IndieReviewEngine, AssembleThreatModelExtrasMissingFiles) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString out = IndieReviewEngine::assembleThreatModelExtras(tmp.path());
    EXPECT_TRUE(out.contains("=== CLAUDE.md ==="));
    EXPECT_TRUE(out.contains("=== SECURITY.md ==="));
    EXPECT_TRUE(out.contains("=== .semgrep.yml ==="));
}

TEST(IndieReviewEngine, AssembleThreatModelExtrasReadsExisting) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    writeFile(tmp.path(), "CLAUDE.md", "Test claude content here.\n");
    writeFile(tmp.path(), "SECURITY.md", "Test security content.\n");
    const QString out = IndieReviewEngine::assembleThreatModelExtras(tmp.path());
    EXPECT_TRUE(out.contains("Test claude content here."));
    EXPECT_TRUE(out.contains("Test security content."));
}
