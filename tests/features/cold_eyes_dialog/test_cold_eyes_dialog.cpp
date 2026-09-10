// ANTS-1721 — ColdEyesDialog feature test (GUI bundle).
//
// Exercises the non-GUI composition / corroboration / fold-in paths of
// ColdEyesDialog : ReviewDialogBase. INV-1..7 drive the engine + brief +
// fold-in logic directly; INV-8 is a construction smoke assertion. INV-6
// and INV-9 lock ANTS-2011 (cold re-review + loop log); INV-9 drives real
// dispatch rounds (ReviewDialogBase::startDispatch / redispatch via a
// synchronous fake LlmDispatcher::JobRunner) rather than calling
// onAllReportsCollected directly, because the loop log must track what
// each round DISPATCHED, not ReviewDialogBase's accumulated reports().
// See tests/features/cold_eyes_dialog/spec.md + docs/specs/ANTS-1721.md.

#include "coldeyesdialog.h"
#include "coldeyesengine.h"
#include "config.h"
#include "reviewdialogbase.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPlainTextEdit>
#include <QPushButton>
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
    // INV-9 — real-dispatch surface (ANTS-2011): drive startDispatch() /
    // the real re-review button with a synchronous fake runner instead of
    // calling onAllReportsCollected directly, so the test exercises
    // ReviewDialogBase::redispatch's merge-into-m_reports path.
    using ColdEyesDialog::setJobRunner;
    using ColdEyesDialog::startDispatch;
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

// INV-6 — re-review covers only finding lanes; the re-review brief runs
// cold (ANTS-2011: no "prior fix" / "do not re-raise" block — a returning
// finding IS the signal that a fix didn't hold, so nothing tells the
// reviewer what to skip).
TEST(ColdEyesDialog, INV6_ReReviewSubsetRunsCold) {
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

    // Re-review brief for the finding lane: cold, no matter what the prior
    // round found. No markFindingFixed call exists any more (ANTS-2011).
    // Assert absence of the prior-fix header text only — NOT "re-raise":
    // falseposledger.cpp's formatForBrief legitimately emits its own
    // "do not re-raise" phrase inside the (unrelated) prior-FP block that
    // INV-2 requires, so that needle would fail for the wrong reason
    // whenever a false-positive ledger entry is present.
    const ReviewLane std = laneById(dlg.derivePartition(), QStringLiteral("standards"));
    const LlmRequest req = dlg.composeBrief(std);
    EXPECT_FALSE(req.userPrompt.contains("Previously fixed"))
        << "re-review must carry no prior-fix block — the reviewer is told "
           "nothing about what happened last round";
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

// INV-9 — each DISPATCHED round (a full dispatch or a re-review) appends
// exactly one loop-log entry: loop number, the lanes THAT ROUND dispatched
// (not reports().keys() — ReviewDialogBase::redispatch merges into the
// accumulated map, so after a re-review that map holds every lane, and
// onAllFinished always hands onAllReportsCollected the whole merged
// thing), and the corroborated/uncorroborated counts after that round's
// collection. The results view renders one "Round N" line per round
// (ANTS-2011).
TEST(ColdEyesDialog, INV9_LoopLogTracksDispatchedRoundsNotMergedReports) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildDocTree(tmp.path());

    Config cfg;
    cfg.setAiEndpoint(QStringLiteral("http://127.0.0.1:9/v1/chat/completions"));
    Dlg dlg(tmp.path(), nullptr, &cfg);

    ASSERT_GE(dlg.engineLanes().size(), 2)
        << "fixture must produce >= 2 lanes so round 1's dispatched-lane "
           "list isn't trivially equal to round 2's single-lane re-review";

    // Synchronous fake runner: round 1 gives "standards" a finding, every
    // other lane an empty (clean) report; round 2 flips "standards" clean
    // too, so round 2's counts differ from round 1's and can't pass by
    // the log simply re-reporting stale numbers.
    int round = 1;
    dlg.setJobRunner([&round](const LlmJob &job,
                              std::function<void(const LlmResult &)> done) {
        LlmResult r; r.ok = true;
        if (job.id == QStringLiteral("standards") && round == 1)
            r.text = QStringLiteral("- [LOW] src/bar.cpp:7 — only here\n");
        else
            r.text = QString();
        done(r);
    });

    // Round 1 — full dispatch of every selected lane.
    dlg.startDispatch();

    // Round 2 — flip the canned reply, then drive the REAL re-review
    // button (not a direct onAllReportsCollected call) so the test
    // exercises ReviewDialogBase::redispatch's merge-into-m_reports path
    // and LlmDispatcher's synchronous-runner re-entrancy, not a shortcut
    // around either.
    round = 2;
    QPushButton *reReviewBtn = nullptr;
    for (QPushButton *b : dlg.findChildren<QPushButton *>())
        if (b->text() == QStringLiteral("Re-review lanes with findings"))
            reReviewBtn = b;
    ASSERT_TRUE(reReviewBtn != nullptr) << "re-review button must exist";
    reReviewBtn->click();

    const QList<ColdEyesDialog::LoopEntry> &log = dlg.loopLog();
    ASSERT_EQ(log.size(), 2)
        << "exactly one entry per dispatched round — also locks against "
           "LlmDispatcher::pump() re-entering and firing allFinished more "
           "than once per round under a synchronous runner";

    QStringList allLaneNames;
    for (const auto &l : dlg.engineLanes()) allLaneNames << l.name;
    allLaneNames.sort();

    EXPECT_EQ(log.at(0).loop, 1) << "first round is loop 1";
    EXPECT_EQ(log.at(0).lanes, allLaneNames)
        << "round 1 was a full dispatch — lanes is every selected lane, "
           "sorted";
    EXPECT_EQ(log.at(0).corroborated, 0);
    EXPECT_EQ(log.at(0).uncorroborated, 1)
        << "single-lane bar.cpp:7 finding, round 1";

    EXPECT_EQ(log.at(1).loop, 2) << "second round is loop 2";
    EXPECT_EQ(log.at(1).lanes, QStringList{QStringLiteral("standards")})
        << "round 2 was a re-review of only the finding lane — lanes must "
           "be the DISPATCHED set, not reports().keys(), which after the "
           "redispatch merge holds every lane from round 1 too";
    EXPECT_EQ(log.at(1).corroborated, 0);
    EXPECT_EQ(log.at(1).uncorroborated, 0)
        << "the finding was fixed — round 2's report is clean";

    bool foundRound1 = false, foundRound2 = false;
    for (QPlainTextEdit *edit : dlg.findChildren<QPlainTextEdit *>()) {
        const QString text = edit->toPlainText();
        if (text.contains("Round 1")) foundRound1 = true;
        if (text.contains("Round 2")) foundRound2 = true;
    }
    EXPECT_TRUE(foundRound1)
        << "loop log must be rendered into a results view: round 1 missing";
    EXPECT_TRUE(foundRound2)
        << "loop log must be rendered into a results view: round 2 missing";
}
