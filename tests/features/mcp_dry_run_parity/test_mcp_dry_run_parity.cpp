// Feature-conformance test for spec.md (ANTS-2227, part 1).
//
// Behavioural tests on ants::falsepos::appendEntry(dryRun) (a free function),
// plus source-scrapes for the per-handler + schema wiring (the handlers need a
// full RemoteControl to run, so they are verified by source-scrape — the same
// posture as tests/features/mcp_apply_edits).

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#include "falseposledger.h"
#include "roadmapfoldin.h"
#include "debtsweepengine.h"

#include <QString>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QList>
#include <string>

namespace {

bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

size_t countOf(const std::string &hay, const std::string &needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

void writeFile(const QString &root, const QString &rel, const QByteArray &body) {
    const QString abs = root + QChar('/') + rel;
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile f(abs);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));  // ANTS-3411 — check nodiscard open()
    f.write(body);
    f.close();
}

QByteArray readFile(const QString &abs) {
    QFile f(abs);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));  // ANTS-3411 — check nodiscard open()
    const QByteArray b = f.readAll();
    f.close();
    return b;
}

ants::falsepos::LedgerEntry validEntry() {
    ants::falsepos::LedgerEntry e;
    e.reviewKind = QStringLiteral("audit");
    e.claim      = QStringLiteral("foo.cpp:10 flagged as unused but is a slot");
    e.rationale  = QStringLiteral("connected via QMetaObject; tool can't see it");
    e.timestamp  = QStringLiteral("2026-06-30");
    e.loggedBy   = QStringLiteral("cc-session");
    return e;
}

}  // namespace

// INV-1 + INV-2 — dry_run leaves the ledger absent, and the would-be byte
// count equals what a subsequent real append writes.
TEST(McpDryRunParity, FalseposDryRunNoWriteThenRealMatches) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    const QString ledger = root + QStringLiteral("/.ants_review_falsepos.jsonl");

    const auto dry = ants::falsepos::appendEntry(root, validEntry(), /*dryRun=*/true);
    EXPECT_TRUE(dry.ok) << dry.message.toStdString();
    EXPECT_TRUE(dry.created);             // file absent → would create
    EXPECT_GT(dry.bytesAppended, 0);
    EXPECT_FALSE(QFileInfo::exists(ledger))
        << "dry_run must not create the ledger file";

    const auto real = ants::falsepos::appendEntry(root, validEntry(), /*dryRun=*/false);
    EXPECT_TRUE(real.ok) << real.message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(ledger)) << "real append must create the ledger";
    EXPECT_EQ(dry.bytesAppended, real.bytesAppended)
        << "dry_run preview must match the real write (shared code path)";
}

// INV-3 — the preview still runs full validation.
TEST(McpDryRunParity, FalseposDryRunValidationStillFires) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ants::falsepos::LedgerEntry bad = validEntry();
    bad.reviewKind = QStringLiteral("not-a-kind");
    const auto r = ants::falsepos::appendEntry(tmp.path(), bad, /*dryRun=*/true);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, QStringLiteral("bad_args"));
    EXPECT_FALSE(QFileInfo::exists(
        tmp.path() + QStringLiteral("/.ants_review_falsepos.jsonl")));
}

// INV-4 — per-handler dry_run gates (source-scrape).
TEST(McpDryRunParity, HandlerGatesWired) {
    const std::string rc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rc.empty());

    // apply_edits — reads dry_run and has a would-write branch before QSaveFile.
    EXPECT_TRUE(has(rc, "const bool dryRun = req.value(QStringLiteral(\"dry_run\")).toBool();"));
    EXPECT_TRUE(has(rc, "if (dryRun) env[\"dry_run\"] = true;"))
        << "apply_edits envelope must carry dry_run:true";
    // project_settings — writeOut dry_run early-return.
    EXPECT_TRUE(has(rc, "o[QStringLiteral(\"would_write\")] = true;"))
        << "project_settings writeOut must preview would_write";
    // feedback_log — pre-write preview envelope.
    EXPECT_TRUE(has(rc, "out[\"bytes_appended\"] = static_cast<qint64>(addedUtf8.size());"));
    // audit_falsepos_log — passes the flag to appendEntry.
    EXPECT_TRUE(has(rc, "ants::falsepos::appendEntry(root, e, dryRun)"))
        << "audit_falsepos_log must thread dry_run into appendEntry";
    // Part 2 fold-in family — peekIds instead of allocateIds + gated insert.
    EXPECT_TRUE(has(rc, "RoadmapFoldIn::peekIds(root, actionable.size())"))
        << "indie_review/cold_eyes fold_in must peek IDs under dry_run";
    EXPECT_TRUE(has(rc, "RoadmapFoldIn::peekIds(root, deferred.size())"))
        << "debt_sweep_defer must peek IDs under dry_run";
    EXPECT_TRUE(has(rc, "? false : RoadmapFoldIn::insertBlock(root, heading, block)"))
        << "fold-in inserts must be skipped under dry_run";
    // Part 3 — debt_sweep_apply_fix threads dry_run into the engine.
    EXPECT_TRUE(has(rc, "DebtSweepEngine::applyMechanicalFix(root, f, dryRun)"))
        << "debt_sweep_apply_fix must thread dry_run into applyMechanicalFix";
    EXPECT_TRUE(has(rc, "env[\"would_apply\"] = v.wouldApply;"))
        << "debt_sweep_apply_fix must surface would_apply under dry_run";
}

// INV-8 (part 3) — test_audit_fold_in is engine + lambda, not a cmd* handler;
// debt_sweep_apply_fix's no-write seam lives in the engine. Source-scrape the
// three files (the engine + lambda + verdict need a full app to run).
TEST(McpDryRunParity, EngineAndLambdaGatesWired) {
    const std::string te = ants_test::slurpFile(SRC_TESTAUDITENGINE_CPP_PATH);
    ASSERT_FALSE(te.empty());
    // TestAuditEngine::foldIn routes peekIds under dry_run + gates both inserts.
    EXPECT_TRUE(has(te, "RoadmapFoldIn::peekIds(canon, n)"))
        << "test_audit foldIn must peek IDs under req.dryRun";
    EXPECT_TRUE(has(te, "req.dryRun"))
        << "test_audit foldIn must branch on req.dryRun";

    const std::string mw = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty());
    // The provider lambda reads dry_run into req and echoes it on success.
    EXPECT_TRUE(has(mw, "req.dryRun        = args.value(QStringLiteral(\"dry_run\")).toBool();"))
        << "test_audit_fold_in lambda must read dry_run into req.dryRun";
    EXPECT_TRUE(has(mw, "if (req.dryRun) env[\"dry_run\"] = true;"))
        << "test_audit_fold_in lambda must echo dry_run:true";

    const std::string ds = ants_test::slurpFile(SRC_DEBTSWEEPENGINE_CPP_PATH);
    ASSERT_FALSE(ds.empty());
    // applyMechanicalFix skips the QSaveFile write under dryRun.
    EXPECT_TRUE(has(ds, "v.wouldApply = true;"))
        << "applyMechanicalFix must set wouldApply under dryRun";
}

// INV-7 (part 3) — applyMechanicalFix(dryRun=true) computes the patch but
// leaves the source byte-identical; the same finding with dryRun=false mutates
// it (preview can't drift — shared validate+patch path).
TEST(McpDryRunParity, ApplyMechanicalFixDryRunNoWriteThenRealMutates) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    const QByteArray before =
        "int main() {\n"
        "    Q_UNUSED(stale);\n"
        "    return 0;\n"
        "}\n";
    writeFile(root, QStringLiteral("src/foo.cpp"), before);
    const QString abs = root + QStringLiteral("/src/foo.cpp");

    DebtSweepEngine::Finding f;
    f.category    = QStringLiteral("code_drift");
    f.detectorId  = QStringLiteral("orphan_q_unused");
    f.file        = QStringLiteral("src/foo.cpp");
    f.line        = 2;
    f.autoFixable = true;

    const auto dry = DebtSweepEngine::applyMechanicalFix(root, f, /*dryRun=*/true);
    EXPECT_TRUE(dry.wouldApply) << dry.errorMessage.toStdString();
    EXPECT_FALSE(dry.applied);
    EXPECT_TRUE(dry.errorCode.isEmpty());
    EXPECT_EQ(readFile(abs), before) << "dry_run must not mutate the source file";

    const auto real = DebtSweepEngine::applyMechanicalFix(root, f, /*dryRun=*/false);
    EXPECT_TRUE(real.applied);
    EXPECT_FALSE(real.wouldApply);
    EXPECT_FALSE(readFile(abs).contains("Q_UNUSED(stale)"))
        << "real apply must delete the marker line";
}

// INV-5 — uniform schema prop factory, declared on all seven new descriptors.
TEST(McpDryRunParity, SchemaPropWired) {
    const std::string ci = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.empty());
    EXPECT_TRUE(has(ci, "auto makeDryRunProp = []"))
        << "makeDryRunProp factory missing";
    // 4 part-1 (apply_edits/project_settings/feedback_log/audit_falsepos_log)
    // + 3 part-2 (indie_review/cold_eyes fold_in + debt_sweep_defer)
    // + 2 part-3 (test_audit_fold_in + debt_sweep_apply_fix).
    EXPECT_GE(countOf(ci, "= makeDryRunProp();"), 9u)
        << "dry_run prop must be declared on all nine new write descriptors";
}

// INV-6 (part 2) — peekIds returns the same IDs allocateIds would, WITHOUT
// bumping the counter.
TEST(McpDryRunParity, PeekIdsMatchesAllocateWithoutBump) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = tmp.path();
    const QString counter = root + QStringLiteral("/.roadmap-counter");
    { QFile f(counter); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("5\n"); }

    const auto peek = RoadmapFoldIn::peekIds(root, 3);
    EXPECT_EQ(peek, (QList<int>{6, 7, 8}));
    { QFile f(counter); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
      EXPECT_EQ(f.readAll().trimmed(), QByteArray("5")); }   // peek did not bump

    const auto alloc = RoadmapFoldIn::allocateIds(root, 3);
    EXPECT_EQ(alloc, peek) << "peek must equal the real allocation";
    { QFile f(counter); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
      EXPECT_EQ(f.readAll().trimmed(), QByteArray("8")); }    // alloc bumped
}

// INV-6 (part 2) — an absent/fresh counter peeks as 1…N (allocateIds parity),
// and the peek never creates the counter file.
TEST(McpDryRunParity, PeekIdsFreshCounterStartsAtOne) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_EQ(RoadmapFoldIn::peekIds(tmp.path(), 2), (QList<int>{1, 2}));
    EXPECT_FALSE(QFileInfo::exists(tmp.path() + QStringLiteral("/.roadmap-counter")))
        << "peek must not create the counter file";
}
