// ANTS-1258 — IndieReviewDialog feature test (GUI bundle).
//
// Exercises the non-GUI composition / corroboration / synthesis / fold-in
// paths of IndieReviewDialog : ReviewDialogBase. INV-1..7 drive the engine
// + brief + fold-in logic directly; INV-8 is a construction smoke
// assertion. See tests/features/indie_review_dialog/spec.md +
// docs/specs/ANTS-1258.md.

#include "indiereviewdialog.h"
#include "indiereviewengine.h"
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

#include <functional>

namespace {

// Subclass exposing the protected ReviewDialogBase hooks the tests drive,
// plus a counter that proves the synthesis follow-up never re-enters
// onAllReportsCollected (INV-5).
class Dlg : public IndieReviewDialog {
public:
    using IndieReviewDialog::IndieReviewDialog;
    using IndieReviewDialog::composeBrief;
    using IndieReviewDialog::derivePartition;
    using IndieReviewDialog::dispatchSynthesis;
    using IndieReviewDialog::performFoldIn;
    using IndieReviewDialog::setJobRunner;

    int onAllCalls = 0;
    void onAllReportsCollected(const QHash<QString, QString> &r) override {
        ++onAllCalls;
        IndieReviewDialog::onAllReportsCollected(r);
    }
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

// A src tree whose CLAUDE.md module map yields a "foo" and a "bar" lane.
// foo.cpp / bar.cpp exist so report citations resolve
// (extractFileLineCitations rejects citations to files not on disk).
void buildProject(const QString &root) {
    writeFile(root + "/CLAUDE.md",
              "# CLAUDE\n\n## Module map (src/)\n\n"
              "- `foo` \xe2\x80\x94 does foo things\n"
              "- `bar` \xe2\x80\x94 does bar things\n\n"
              "## Other\n");
    writeFile(root + "/src/foo.h", "int foo();\n");
    writeFile(root + "/src/foo.cpp", "int foo() { return 0; } // FOOBODY\n");
    writeFile(root + "/src/bar.h", "int bar();\n");
    writeFile(root + "/src/bar.cpp", "int bar() { return 1; }\n");
    writeFile(root + "/ROADMAP.md", "# Roadmap\n");
}

ReviewLane laneById(const QList<ReviewLane> &lanes, const QString &id) {
    for (const ReviewLane &l : lanes)
        if (l.id == id) return l;
    return {};
}

const IndieReviewEngine::Lane *engLane(const QList<IndieReviewEngine::Lane> &v,
                                       const QString &name) {
    for (const auto &l : v)
        if (l.name == name) return &l;
    return nullptr;
}

}  // namespace

// INV-1 — lanes mirror the engine; a deselected lane drops out.
TEST(IndieReviewDialog, INV1_PartitionMirrorsEngineAndHonoursDeselect) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);

    const auto engine = IndieReviewEngine::derivePartition(tmp.path());
    QSet<QString> engineNames;
    for (const auto &l : engine) engineNames.insert(l.name);

    QSet<QString> dialogNames;
    for (const ReviewLane &l : dlg.derivePartition()) dialogNames.insert(l.id);
    EXPECT_EQ(dialogNames, engineNames) << "lane set must match the engine";
    ASSERT_TRUE(engineNames.contains("foo"))
        << "fixture should produce a foo lane";

    dlg.setLaneSelected(QStringLiteral("foo"), false);
    QSet<QString> afterDeselect;
    for (const ReviewLane &l : dlg.derivePartition()) afterDeselect.insert(l.id);
    EXPECT_FALSE(afterDeselect.contains("foo"))
        << "deselected lane must not be dispatched";
    EXPECT_TRUE(afterDeselect.contains("bar")) << "other lanes remain";
}

// INV-2 — brief reuses assembleBriefForDispatch: fenced bodies, inlined
// standards, exactly one prior-FP block.
TEST(IndieReviewDialog, INV2_BriefReusesDispatchBriefSingleFp) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());
    writeFile(tmp.path() + "/docs/standards/coding.md",
              "# Coding standard\n\nUSE_SPACES_MARKER\n");
    // Seed an indie-review / foo-lane false positive.
    writeFile(tmp.path() + "/.ants_review_falsepos.jsonl",
              "{\"review_kind\":\"indie-review\",\"lane\":\"foo\","
              "\"claim\":\"UNIQUEFPCLAIM in foo.cpp\","
              "\"rationale\":\"intentional per ADR\","
              "\"timestamp\":\"2026-05-20\"}\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    const ReviewLane foo =
        laneById(dlg.derivePartition(), QStringLiteral("foo"));
    ASSERT_FALSE(foo.id.isEmpty());

    const LlmRequest req = dlg.composeBrief(foo);
    const QString &p = req.userPrompt;
    EXPECT_TRUE(p.contains("FOOBODY")) << "source body must be inlined";
    EXPECT_TRUE(p.contains("````")) << "source body must be 4-backtick fenced";
    EXPECT_TRUE(p.contains("USE_SPACES_MARKER"))
        << "standards docs must be inlined";
    EXPECT_EQ(p.count(QStringLiteral("UNIQUEFPCLAIM")), 1)
        << "prior-FP block present exactly once (not duplicated by dialog)";
}

// INV-3 — prompt sum-capped with a marker; small lane passes through.
TEST(IndieReviewDialog, INV3_PromptCapped) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);

    // Small lane: byte-identical to assembleBriefForDispatch.
    {
        const ReviewLane bar =
            laneById(dlg.derivePartition(), QStringLiteral("bar"));
        ASSERT_FALSE(bar.id.isEmpty());
        const auto eng = IndieReviewEngine::derivePartition(tmp.path());
        const IndieReviewEngine::Lane *el = engLane(eng, QStringLiteral("bar"));
        ASSERT_NE(el, nullptr);
        const QString direct =
            IndieReviewEngine::assembleBriefForDispatch(tmp.path(), *el);
        const LlmRequest req = dlg.composeBrief(bar);
        EXPECT_EQ(req.userPrompt, direct)
            << "small lane prompt must be unmodified";
    }

    // Oversize lane: a single >200 KiB source body trips the cap.
    {
        const QString big = QString(250 * 1024, QChar('a'));
        writeFile(tmp.path() + "/src/foo.cpp",
                  "int foo() { /* " + big + " */ return 0; }\n");
        const ReviewLane foo =
            laneById(dlg.derivePartition(), QStringLiteral("foo"));
        ASSERT_FALSE(foo.id.isEmpty());
        const LlmRequest req = dlg.composeBrief(foo);
        EXPECT_LE(req.userPrompt.toUtf8().size(),
                  IndieReviewDialog::kPromptCapBytes)
            << "prompt must respect the 200 KiB sum cap";
        EXPECT_TRUE(req.userPrompt.contains("truncated"))
            << "a truncation marker must be present";
    }
}

// INV-4 — corroboration at minLanes=2; single-lane cites kept separately.
TEST(IndieReviewDialog, INV4_CorroborationAndUncorroborated) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());
    Dlg dlg(tmp.path(), nullptr, nullptr);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("foo"),
                   QStringLiteral("- [HIGH] src/foo.cpp:42 — drift\n"));
    reports.insert(QStringLiteral("bar"),
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
    EXPECT_TRUE(uncorrBar)
        << "single-lane bar.cpp:7 → uncorroborated, not dropped";
}

// INV-5 — synthesis runs via dispatchOne and never re-enters
// onAllReportsCollected.
TEST(IndieReviewDialog, INV5_SynthesisViaDispatchOneNoReentry) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());
    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setJobRunner([](const LlmJob &,
                        std::function<void(const LlmResult &)> done) {
        LlmResult r;
        r.ok = true;
        r.text = QStringLiteral("SYNTH-SUMMARY");
        done(r);
    });

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("foo"),
                   QStringLiteral("- [HIGH] src/foo.cpp:1 — x\n"));
    reports.insert(QStringLiteral("bar"),
                   QStringLiteral("- [HIGH] src/foo.cpp:1 — x\n"));
    dlg.onAllReportsCollected(reports);   // null config → no auto synthesis
    ASSERT_EQ(dlg.onAllCalls, 1);

    dlg.dispatchSynthesis();              // explicit, with stub runner
    EXPECT_EQ(dlg.synthesisText(), QStringLiteral("SYNTH-SUMMARY"))
        << "synthesis result must be captured";
    EXPECT_EQ(dlg.onAllCalls, 1)
        << "synthesis must not re-enter onAllReportsCollected";
}

// INV-6 — re-review covers only finding lanes.
TEST(IndieReviewDialog, INV6_ReReviewSubset) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());
    Dlg dlg(tmp.path(), nullptr, nullptr);

    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("foo"),
                   QStringLiteral("- [HIGH] src/foo.cpp:42 — issue\n"));
    reports.insert(QStringLiteral("bar"), QString());  // clean
    dlg.onAllReportsCollected(reports);

    const QStringList toRe = dlg.lanesToReReview();
    EXPECT_TRUE(toRe.contains("foo")) << "lane with findings re-reviewed";
    EXPECT_FALSE(toRe.contains("bar")) << "clean lane skipped";
}

// INV-7 — narrative fold-in: no IDs; per-finding: one ID per finding.
TEST(IndieReviewDialog, INV7_FoldInIdAllocation) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());
    writeFile(tmp.path() + "/.roadmap-counter", "100\n");
    writeFile(tmp.path() + "/ROADMAP.md",
              "# Roadmap\n\n## 0.7.90 — current (target: 2026-05)\n\n"
              "### Existing\n- bullet\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    // Two corroborated findings (each cited by both lanes).
    QHash<QString, QString> reports;
    reports.insert(QStringLiteral("foo"),
                   QStringLiteral("- [HIGH] src/foo.cpp:1 — a\n"
                                  "- [MED] src/bar.cpp:2 — b\n"));
    reports.insert(QStringLiteral("bar"),
                   QStringLiteral("- [HIGH] src/foo.cpp:1 — a\n"
                                  "- [MED] src/bar.cpp:2 — b\n"));
    dlg.onAllReportsCollected(reports);
    ASSERT_EQ(dlg.results().corroborated.size(), 2);

    // Narrative mode → counter untouched.
    dlg.setFoldInMode(IndieReviewDialog::FoldInMode::Narrative);
    dlg.setNarrativeText(QStringLiteral("## Closed inline\n- fixed in review\n"));
    dlg.performFoldIn();
    EXPECT_EQ(readFile(tmp.path() + "/.roadmap-counter").trimmed(),
              QStringLiteral("100"))
        << "narrative fold-in must not allocate IDs";

    // Per-finding mode → counter advances by N (2).
    dlg.setFoldInMode(IndieReviewDialog::FoldInMode::PerFinding);
    dlg.performFoldIn();
    EXPECT_EQ(readFile(tmp.path() + "/.roadmap-counter").trimmed(),
              QStringLiteral("102"))
        << "per-finding fold-in allocates one ID per finding";
}

// INV-8 — smoke: unset endpoint → dispatch disabled; constructs cleanly.
TEST(IndieReviewDialog, INV8_DispatchDisabledSmoke) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    buildProject(tmp.path());
    EXPECT_FALSE(ReviewDialogBase::endpointDispatchable(QString()));
    Dlg dlg(tmp.path(), nullptr, nullptr);  // null config = no endpoint
    SUCCEED();
}
