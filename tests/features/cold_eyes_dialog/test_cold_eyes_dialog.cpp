// ANTS-1721 — ColdEyesDialog feature test (GUI bundle).
//
// Exercises the non-GUI composition / corroboration / fold-in paths of
// ColdEyesDialog : ReviewDialogBase. INV-1..7 drive the engine + brief +
// fold-in logic directly; INV-8 is a construction smoke assertion.
// See tests/features/cold_eyes_dialog/spec.md + docs/specs/ANTS-1721.md.

#include "coldeyesdialog.h"
#include "coldeyesengine.h"
#include "reviewdialogbase.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Subclass exposing the protected ReviewDialogBase hooks the tests drive.
// The ColdEyesDialog control/accessor surface is already public.
class Dlg : public ColdEyesDialog {
public:
    using ColdEyesDialog::ColdEyesDialog;
    using ColdEyesDialog::composeBrief;
    using ColdEyesDialog::derivePartition;
    using ColdEyesDialog::onAllReportsCollected;
    using ColdEyesDialog::performFoldIn;
};

bool writeFile(const QString &path, const QString &body) {
    QFileInfo fi(path);
    QDir().mkpath(fi.absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(body.toUtf8());
    return true;
}

QString readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

// A doc tree that yields at least a "contracts" lane and a "standards"
// lane from ColdEyesEngine::derivePartition. `src/foo.cpp` + `src/bar.cpp`
// exist so report citations resolve (extractFileLineCitations rejects
// citations to files not on disk).
void buildDocTree(const QString &root) {
    writeFile(root + "/CLAUDE.md", "# CLAUDE\n\nProject guide.\n");
    writeFile(root + "/README.md", "# Readme\n\nOverview.\n");
    writeFile(root + "/CHANGELOG.md", "# Changelog\n\n## 0.1.0\n");
    writeFile(root + "/ROADMAP.md",
              "# Roadmap\n\n## 0.7.90 — current (target: 2026-05)\n\n"
              "### Standards detail section\n- standards work item\n\n"
              "## Qzxwv\n- plugh xyzzy frobnitz\n");
    writeFile(root + "/docs/standards/coding.md",
              "# Coding standard\n\nUse spaces. See src/foo.cpp:42 for an "
              "example.\n");
    writeFile(root + "/src/foo.cpp", "int foo() { return 0; }\n");
    writeFile(root + "/src/bar.cpp", "int bar() { return 1; }\n");
}

ReviewLane laneById(const QList<ReviewLane> &lanes, const QString &id) {
    for (const ReviewLane &l : lanes)
        if (l.id == id) return l;
    return {};
}

}  // namespace

// INV-1 — lanes mirror the engine; a deselected lane drops out.
TEST(ColdEyesDialog, INV1_PartitionMirrorsEngineAndHonoursDeselect) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);

    const auto engine = ColdEyesEngine::derivePartition(tmp.path());
    QSet<QString> engineNames;
    for (const auto &l : engine.lanes) engineNames.insert(l.name);

    QSet<QString> dialogNames;
    for (const ReviewLane &l : dlg.derivePartition()) dialogNames.insert(l.id);
    EXPECT_EQ(dialogNames, engineNames) << "lane set must match the engine";
    ASSERT_TRUE(engineNames.contains("standards"))
        << "fixture should produce a standards lane";

    dlg.setLaneSelected(QStringLiteral("standards"), false);
    QSet<QString> afterDeselect;
    for (const ReviewLane &l : dlg.derivePartition()) afterDeselect.insert(l.id);
    EXPECT_FALSE(afterDeselect.contains("standards"))
        << "deselected lane must not be dispatched";
    EXPECT_TRUE(afterDeselect.contains("contracts"))
        << "other lanes remain";
}

// INV-2 — brief inlines lane bodies whole, narrows cross-refs by keyword,
// and includes the prior-FP block.
TEST(ColdEyesDialog, INV2_BriefInlinesBodiesNarrowsCrossRefAndFP) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    // Seed a cold-eyes / standards-lane false positive.
    writeFile(tmp.path() + "/.ants_review_falsepos.jsonl",
              "{\"review_kind\":\"cold-eyes\",\"lane\":\"standards\","
              "\"claim\":\"UNIQUEFPCLAIM in coding.md\","
              "\"rationale\":\"intentional per ADR\","
              "\"timestamp\":\"2026-05-20\"}\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    const ReviewLane std = laneById(dlg.derivePartition(), QStringLiteral("standards"));
    ASSERT_FALSE(std.id.isEmpty());

    const LlmRequest req = dlg.composeBrief(std);
    const QString &p = req.userPrompt;
    // Lane body inlined whole.
    EXPECT_TRUE(p.contains("Use spaces.")) << "lane doc body must be inlined";
    // Cross-ref narrowed to the keyword-matching section only.
    EXPECT_TRUE(p.contains("Standards detail section"))
        << "keyword-matching cross-ref section must be present";
    EXPECT_FALSE(p.contains("plugh xyzzy frobnitz"))
        << "non-matching cross-ref section must be excluded";
    // Prior-FP block present.
    EXPECT_TRUE(p.contains("UNIQUEFPCLAIM"))
        << "prior false-positive must be surfaced";
}

// INV-3 — stale citation becomes an accuracy finding with no model report.
TEST(ColdEyesDialog, INV3_StaleCitationSurfacesWithEmptyReport) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    // coding.md cites a missing src file.
    writeFile(tmp.path() + "/docs/standards/coding.md",
              "# Coding\n\nSee src/missing_xyz.cpp:10 — does not exist.\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    const ReviewLane std = laneById(dlg.derivePartition(), QStringLiteral("standards"));
    ASSERT_FALSE(std.id.isEmpty());

    (void)dlg.composeBrief(std);                  // populates stale citations
    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("standards"), QString());  // empty report
    dlg.onAllReportsCollected(reports);

    bool found = false;
    for (const QString &s : dlg.results().staleFindings)
        if (s.contains("missing_xyz.cpp")) found = true;
    EXPECT_TRUE(found) << "stale citation must surface as accuracy finding";
}

// INV-4 — prompt sum-capped; cross-ref dropped before lane bodies.
TEST(ColdEyesDialog, INV4_PromptCappedDropsCrossRefFirst) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    // BriefDispatch caps each lane body at 64 KiB and each cross-ref doc at
    // 32 KiB, so the 200 KiB *sum* gate only fires across several files.
    // Three standards docs (each clamped to 64 KiB ⇒ ~192 KiB lane bodies)
    // plus a 64 KiB matching cross-ref section pushes the sum over 200 KiB
    // while the fixed (lane) part alone stays under it.
    const QString big = QString(70000, QChar('a'));
    writeFile(tmp.path() + "/docs/standards/coding.md",
              "# Coding standard\n\nLANEBODYUNIQUE " + big + "\n");
    writeFile(tmp.path() + "/docs/standards/testing.md",
              "# Testing standard\n\n" + big + "\n");
    writeFile(tmp.path() + "/docs/standards/documentation.md",
              "# Documentation standard\n\n" + big + "\n");
    const QString xrefFill = QString(64 * 1024, QChar('b'));
    writeFile(tmp.path() + "/ROADMAP.md",
              "# Roadmap\n\n## 0.7.90 — current (target: 2026-05)\n\n"
              "### Standards detail XREFUNIQUE\n" + xrefFill + "\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    const ReviewLane std = laneById(dlg.derivePartition(), QStringLiteral("standards"));
    ASSERT_FALSE(std.id.isEmpty());

    const LlmRequest req = dlg.composeBrief(std);
    EXPECT_LE(req.userPrompt.toUtf8().size(), ColdEyesDialog::kPromptCapBytes)
        << "prompt must respect the 200 KiB sum cap";
    EXPECT_TRUE(req.userPrompt.contains("LANEBODYUNIQUE"))
        << "lane bodies must be retained";
    EXPECT_FALSE(req.userPrompt.contains("XREFUNIQUE"))
        << "cross-ref excerpts dropped first when over budget";
    EXPECT_TRUE(req.userPrompt.contains("truncated"))
        << "a truncation marker must be present";
}

// INV-5 — corroboration at minLanes=2; single-lane cites kept separately.
TEST(ColdEyesDialog, INV5_CorroborationAndUncorroborated) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    Dlg dlg(tmp.path(), nullptr, nullptr);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("contracts"),
                   QStringLiteral("- [HIGH] src/foo.cpp:42 — drift\n"));
    reports.insert(QStringLiteral("standards"),
                   QStringLiteral("- [HIGH] src/foo.cpp:42 — same drift\n"
                                  "- [LOW] src/bar.cpp:7 — only here\n"));
    dlg.onAllReportsCollected(reports);

    bool corrFoo = false;
    for (const auto &f : dlg.results().corroborated)
        if (f.file.contains("foo.cpp") && f.line == 42) corrFoo = true;
    EXPECT_TRUE(corrFoo) << "two lanes citing foo.cpp:42 → corroborated";

    bool uncorrBar = false;
    for (const auto &f : dlg.results().uncorroborated)
        if (f.file.contains("bar.cpp") && f.line == 7) uncorrBar = true;
    EXPECT_TRUE(uncorrBar) << "single-lane bar.cpp:7 → uncorroborated, not dropped";
}

// INV-6 — re-review covers only finding lanes; fixes thread forward.
TEST(ColdEyesDialog, INV6_ReReviewSubsetAndPriorFix) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    Dlg dlg(tmp.path(), nullptr, nullptr);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("standards"),
                   QStringLiteral("- [HIGH] src/foo.cpp:42 — issue\n"));
    reports.insert(QStringLiteral("contracts"), QString());  // clean
    dlg.onAllReportsCollected(reports);

    const QStringList toRe = dlg.lanesToReReview();
    EXPECT_TRUE(toRe.contains("standards")) << "lane with findings re-reviewed";
    EXPECT_FALSE(toRe.contains("contracts")) << "clean lane skipped";

    ASSERT_FALSE(dlg.results().corroborated.isEmpty()
                 && dlg.results().uncorroborated.isEmpty());
    const auto f = dlg.results().uncorroborated.isEmpty()
                       ? dlg.results().corroborated.first()
                       : dlg.results().uncorroborated.first();
    dlg.markFindingFixed(f);

    const ReviewLane std = laneById(dlg.derivePartition(), QStringLiteral("standards"));
    const LlmRequest req = dlg.composeBrief(std);
    EXPECT_TRUE(req.userPrompt.contains("foo.cpp"))
        << "fixed finding must thread into the next brief as a prior fix";
    EXPECT_TRUE(req.userPrompt.contains("re-raise"))
        << "prior-fix section header present";
}

// INV-7 — narrative fold-in: no IDs; per-finding: one ID per finding.
TEST(ColdEyesDialog, INV7_FoldInIdAllocation) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    writeFile(tmp.path() + "/.roadmap-counter", "100\n");
    // buildDocTree wrote a ROADMAP with an in-flight heading already.

    Dlg dlg(tmp.path(), nullptr, nullptr);
    // Two corroborated findings (each cited by two lanes).
    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("contracts"),
                   QStringLiteral("- [HIGH] src/foo.cpp:1 — a\n"
                                  "- [MED] src/bar.cpp:2 — b\n"));
    reports.insert(QStringLiteral("standards"),
                   QStringLiteral("- [HIGH] src/foo.cpp:1 — a\n"
                                  "- [MED] src/bar.cpp:2 — b\n"));
    dlg.onAllReportsCollected(reports);
    ASSERT_EQ(dlg.results().corroborated.size(), 2);

    // Narrative mode → counter untouched.
    dlg.setFoldInMode(ColdEyesDialog::FoldInMode::Narrative);
    dlg.setNarrativeText(QStringLiteral("## Closed inline\n- fixed during review\n"));
    dlg.performFoldIn();
    EXPECT_EQ(readFile(tmp.path() + "/.roadmap-counter").trimmed(),
              QStringLiteral("100"))
        << "narrative fold-in must not allocate IDs";

    // Per-finding mode → counter advances by N (2).
    dlg.setFoldInMode(ColdEyesDialog::FoldInMode::PerFinding);
    dlg.performFoldIn();
    EXPECT_EQ(readFile(tmp.path() + "/.roadmap-counter").trimmed(),
              QStringLiteral("102"))
        << "per-finding fold-in allocates one ID per finding";
}

// INV-8 — smoke: unset endpoint → dispatch disabled; constructs cleanly.
TEST(ColdEyesDialog, INV8_DispatchDisabledSmoke) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());
    EXPECT_FALSE(ReviewDialogBase::endpointDispatchable(QString()));
    Dlg dlg(tmp.path(), nullptr, nullptr);  // null config = no endpoint
    SUCCEED();
}
