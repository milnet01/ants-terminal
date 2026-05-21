// ANTS-1722 — TestAuditDialog feature test (GUI bundle).
//
// Exercises the non-GUI partition / brief / synthesis / fold-in / resume
// paths of TestAuditDialog : ReviewDialogBase. INV-8 is a construction
// smoke. See tests/features/test_audit_dialog/spec.md + ANTS-1722.md.

#include "testauditdialog.h"
#include "testauditengine.h"
#include "reviewdialogbase.h"
#include "sessionmemoryengine.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

class Dlg : public TestAuditDialog {
public:
    using TestAuditDialog::TestAuditDialog;
    using TestAuditDialog::composeBrief;
    using TestAuditDialog::derivePartition;
    using TestAuditDialog::onAllReportsCollected;
    using TestAuditDialog::performFoldIn;
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

// Minimal pytest-shaped suite so partition detects a framework. The
// pyproject.toml is the framework signal file (without it the engine
// returns no_tests_found).
void buildSuite(const QString &root) {
    writeFile(root + "/pyproject.toml", "[tool.pytest.ini_options]\n");
    writeFile(root + "/tests/test_a.py",
              "def test_a():\n    assert True  # UNIQUEBODYMARKER\n");
    writeFile(root + "/tests/test_b.py",
              "def test_b():\n    assert 1 == 1\n");
}

}  // namespace

// INV-1 — chunks mirror the engine; the token routes through brief.
TEST(TestAuditDialog, INV1_PartitionMirrorsEngineAndTokenRoutes) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());

    TestAuditEngine::PartitionRequest preq;
    preq.callerCwd = tmp.path();
    const auto engine = TestAuditEngine::partition(preq);
    ASSERT_TRUE(engine.ok) << engine.error.toStdString();

    QSet<QString> engineIds, dialogIds;
    for (const auto &c : engine.chunks) engineIds.insert(c.id);
    for (const auto &c : dlg.chunks()) dialogIds.insert(c.id);
    EXPECT_EQ(dialogIds, engineIds);
    EXPECT_FALSE(dlg.partitionToken().isEmpty());

    ASSERT_FALSE(dlg.chunks().isEmpty());
    const auto b = dlg.briefFor(dlg.chunks().first().id);
    EXPECT_TRUE(b.ok) << "brief with the stored token must succeed: "
                      << b.code.toStdString();
    EXPECT_EQ(b.chunkId, dlg.chunks().first().id);
}

// INV-2 — full active dimensions + fenced source bodies in the prompt.
TEST(TestAuditDialog, INV2_BriefSeedsDimensionsAndInlinesBodies) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());
    const auto lanes = dlg.derivePartition();
    ASSERT_FALSE(lanes.isEmpty());

    const LlmRequest req = dlg.composeBrief(lanes.first());
    ASSERT_FALSE(dlg.dimensionsActive().isEmpty());
    for (const QString &d : dlg.dimensionsActive())
        EXPECT_TRUE(req.userPrompt.contains(d))
            << "missing active dimension: " << d.toStdString();
    EXPECT_TRUE(req.userPrompt.contains("UNIQUEBODYMARKER"))
        << "chunk source body must be inlined";
}

// INV-3 — reports written under the token dir; synth reads them all.
TEST(TestAuditDialog, INV3_ReportsWrittenAndSynthesized) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());
    ASSERT_FALSE(dlg.chunks().isEmpty());

    QHash<QString, QString> reports;
    for (const auto &c : dlg.chunks())
        reports.insert(c.id,
                       QStringLiteral("## flakiness\n- [HIGH] tests/test_a.py:1 — x\n"));
    dlg.onAllReportsCollected(reports);

    const QString relDir = QStringLiteral(".audit_cache/test_audit_")
                           + dlg.partitionToken();
    const QString absDir = tmp.path() + QChar('/') + relDir;
    EXPECT_TRUE(QFileInfo(absDir).isDir()) << "token reports dir must exist";
    for (const auto &c : dlg.chunks())
        EXPECT_TRUE(QFileInfo(absDir + QChar('/') + c.id + ".md").exists())
            << "report file missing for " << c.id.toStdString();
    EXPECT_TRUE(dlg.lastSynth().ok) << dlg.lastSynth().code.toStdString();
    EXPECT_EQ(dlg.lastSynth().reportsRead, dlg.chunks().size());
}

// INV-4 — prompt capped with a marker when bodies exceed the budget.
TEST(TestAuditDialog, INV4_PromptCapped) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    // Four big test files (each clamped to 64 KiB) ⇒ ~256 KiB of bodies
    // in one chunk, over the 200 KiB sum cap.
    writeFile(tmp.path() + "/pyproject.toml", "[tool.pytest.ini_options]\n");
    const QString big = QString(70000, QChar('a'));
    for (int i = 0; i < 4; ++i)
        writeFile(tmp.path() + QStringLiteral("/tests/test_big_%1.py").arg(i),
                  "def test():\n    x = '" + big + "'\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());
    const auto lanes = dlg.derivePartition();
    ASSERT_FALSE(lanes.isEmpty());

    const LlmRequest req = dlg.composeBrief(lanes.first());
    EXPECT_LE(req.userPrompt.toUtf8().size(), TestAuditDialog::kPromptCapBytes);
    EXPECT_TRUE(req.userPrompt.contains("truncated"))
        << "a truncation marker must be present";
}

// INV-5 — fold-in ID allocation: narrative none, per-finding one per item.
TEST(TestAuditDialog, INV5_FoldInIdAllocation) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());
    writeFile(tmp.path() + "/.roadmap-counter", "100\n");
    writeFile(tmp.path() + "/ROADMAP.md",
              "# Roadmap\n\n## 0.7.90 — current (target: 2026-05)\n\n"
              "### Existing\n- bullet\n");

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());

    // Narrative → counter untouched.
    dlg.setFoldInMode(TestAuditDialog::FoldInMode::Narrative);
    dlg.setNarrativeText(QStringLiteral("## Closed inline\n- fixed flake\n"));
    dlg.performFoldIn();
    EXPECT_EQ(readFile(tmp.path() + "/.roadmap-counter").trimmed(),
              QStringLiteral("100"));

    // Per-finding with 2 actionable items → counter advances by 2.
    QJsonArray actionable;
    for (int i = 0; i < 2; ++i) {
        QJsonObject o;
        o["dimension"] = "flakiness";
        o["severity"]  = "HIGH";
        o["file"]      = "tests/test_a.py";
        o["line"]      = i + 1;
        o["summary"]   = "issue";
        o["fix"]       = "fix it";
        actionable.append(o);
    }
    dlg.setActionable(actionable);
    dlg.setFoldInMode(TestAuditDialog::FoldInMode::PerFinding);
    dlg.performFoldIn();
    EXPECT_EQ(readFile(tmp.path() + "/.roadmap-counter").trimmed(),
              QStringLiteral("102"));
}

// INV-6 — stale token → re-partition, no crash, collected reports kept.
TEST(TestAuditDialog, INV6_StaleTokenRepartitions) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());
    ASSERT_FALSE(dlg.chunks().isEmpty());

    QHash<QString, QString> reports;
    reports.insert(dlg.chunks().first().id, QStringLiteral("## finding\n"));
    dlg.onAllReportsCollected(reports);
    ASSERT_FALSE(dlg.collectedReports().isEmpty());

    dlg.setPartitionTokenForTest(QStringLiteral("garbage-token-xyz"));
    const auto b = dlg.briefFor(dlg.chunks().first().id);
    EXPECT_TRUE(b.ok) << "stale token must re-partition + retry: "
                      << b.code.toStdString();
    EXPECT_NE(dlg.partitionToken(), QStringLiteral("garbage-token-xyz"));
    EXPECT_FALSE(dlg.collectedReports().isEmpty())
        << "collected reports must survive the re-partition";
}

// INV-7 — resume reloads collected ids; only un-reviewed chunks remain.
TEST(TestAuditDialog, INV7_ResumeDispatchesUnreviewedOnly) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());

    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());
    ASSERT_FALSE(dlg.chunks().isEmpty());
    const QString firstChunk = dlg.chunks().first().id;

    // Seed session_memory directly with the first chunk already collected.
    QJsonObject o;
    o["partition_token"]    = dlg.partitionToken();
    o["dimensions"]         = QJsonArray::fromStringList(dlg.dimensionsActive());
    QJsonArray collected; collected.append(firstChunk);
    o["collected_chunk_ids"] = collected;
    const auto set = SessionMemoryEngine::execute(
        tmp.path(), SessionMemoryEngine::Op::Set,
        QStringLiteral("test_audit_resume"), o, mem.path());
    ASSERT_TRUE(set.ok) << set.code.toStdString();

    ASSERT_TRUE(dlg.loadResume());
    const QStringList unreviewed = dlg.unreviewedChunkIds();
    EXPECT_FALSE(unreviewed.contains(firstChunk))
        << "already-collected chunk must not be re-dispatched";
    // Every other chunk is still pending.
    for (const auto &c : dlg.chunks()) {
        if (c.id != firstChunk) {
            EXPECT_TRUE(unreviewed.contains(c.id));
        }
    }
}

// INV-8 — smoke: unset endpoint → dispatch disabled; constructs cleanly.
TEST(TestAuditDialog, INV8_DispatchDisabledSmoke) {
    QTemporaryDir tmp; ASSERT_TRUE(tmp.isValid());
    QTemporaryDir mem; ASSERT_TRUE(mem.isValid());
    buildSuite(tmp.path());
    EXPECT_FALSE(ReviewDialogBase::endpointDispatchable(QString()));
    Dlg dlg(tmp.path(), nullptr, nullptr);
    dlg.setSessionMemBaseDir(mem.path());
    SUCCEED();
}
