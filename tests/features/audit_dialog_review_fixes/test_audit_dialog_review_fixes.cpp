// ANTS-6621 — the smaller audit-dialog bugs the 2026-11-06 reviews found.
// Contract: spec.md beside this file.

#include "auditdialog.h"
#include "auditengine.h"
#include "config.h"

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#include "../../_support/xdg_guard.h"

#include <QString>
#include <QTemporaryDir>

namespace {

QString auditSource() {
    return QString::fromStdString(
        ants_test::stripComments(ants_test::slurpAuditDialog()));
}

// The text from `from` up to the next `to` after it; empty when either is
// missing.
QString between(const QString &src, const QString &from, const QString &to) {
    const qsizetype a = src.indexOf(from);
    if (a < 0) return QString();
    const qsizetype b = src.indexOf(to, a);
    if (b < 0) return QString();
    return src.mid(a, b - a);
}

QString functionText(const QString &src, const char *signature) {
    return between(src, QString::fromLatin1(signature), QStringLiteral("\n}\n"));
}

}  // namespace

// INV-1 — row 292
TEST(AuditDialogReviewFixes, FilteredRunWritesNoTrendSnapshot) {
    const QString finish = functionText(auditSource(), "void AuditDialog::finishRun() {");
    ASSERT_FALSE(finish.isEmpty()) << "finishRun not found";
    const qsizetype save = finish.indexOf(QStringLiteral("appendSnapshot("));
    ASSERT_GE(save, 0) << "finishRun does not save a snapshot";
    const qsizetype gate = finish.lastIndexOf(QStringLiteral("if ("), save);
    ASSERT_GE(gate, 0) << "the snapshot save has no gate";
    const QString cond = finish.mid(gate, finish.indexOf(QChar('{'), gate) - gate);
    for (const char *flag : {"!m_snapshotPersisted", "!m_showNewOnly",
                             "!m_sinceBaseline", "!m_recentOnly"})
        EXPECT_TRUE(cond.contains(QLatin1String(flag)))
            << "a filtered count is written to trend.json as the run's total; "
               "gate lacks " << flag << ": " << cond.toStdString();
}

// INV-2 — row 294
TEST(AuditDialogReviewFixes, BlameReadsTheWorkingTree) {
    const QString src = auditSource();
    const QString blame = functionText(src, "void AuditDialog::startQueuedBlame() {");
    ASSERT_FALSE(blame.isEmpty()) << "startQueuedBlame not found";
    ASSERT_TRUE(blame.contains(QStringLiteral("--line-porcelain")));
    EXPECT_FALSE(blame.contains(QStringLiteral("QStringLiteral(\"HEAD\")")))
        << "blame reads HEAD while findings carry working-tree line numbers";
    const QString run = functionText(src, "void AuditDialog::runAudit() {");
    ASSERT_FALSE(run.isEmpty()) << "runAudit not found";
    EXPECT_TRUE(run.contains(QStringLiteral("m_blameCache.clear();")))
        << "a new run reuses the last run's blame for lines that have moved";
}

// INV-3 — row 295
TEST(AuditDialogReviewFixes, DebtPromptDropsTheFindingByContent) {
    const QString body = functionText(auditSource(),
        "void AuditDialog::onDebtAnchorClicked(const QUrl &url) {");
    ASSERT_FALSE(body.isEmpty()) << "onDebtAnchorClicked not found";
    const qsizetype prompt = body.indexOf(QStringLiteral("QInputDialog::getText("));
    ASSERT_GE(prompt, 0) << "the allow prompt not found";
    EXPECT_FALSE(body.mid(prompt).contains(QStringLiteral("removeAt(idx)")))
        << "an index taken before a modal prompt removes whatever a re-scan "
           "put there meanwhile";
    EXPECT_EQ(body.count(QStringLiteral("dropDebtFinding(m_debtFindings, f)")), 2)
        << "the allow and defer paths do not drop the finding by content";
}

// INV-4 — row 298
TEST(AuditDialogReviewFixes, ClaudeHandoffIsScrubbed) {
    const QString region = between(auditSource(),
        QStringLiteral("connect(m_reviewBtn, &QPushButton::clicked"),
        QStringLiteral("emit reviewRequested("));
    ASSERT_FALSE(region.isEmpty()) << "the Review-with-Claude handler not found";
    EXPECT_TRUE(region.contains(QStringLiteral("SecretRedact::scrub(plainTextResults())")))
        << "the hand-off file carries secrets_scan and gitleaks snippets verbatim";
}

// INV-5 and INV-6 — rows 299 and 300
TEST(AuditDialogReviewFixes, SarifSuppressionAndFingerprint) {
    const QString sarif = functionText(auditSource(), "QString AuditDialog::exportSarif() const {");
    ASSERT_FALSE(sarif.isEmpty()) << "exportSarif not found";
    EXPECT_FALSE(sarif.contains(QStringLiteral("if (reason.isEmpty()) reason = f.aiReasoning;")))
        << "a TRUE_POSITIVE rationale is exported as a suppression's justification";
    const qsizetype fallback = sarif.indexOf(QStringLiteral("reason = f.aiReasoning;"));
    ASSERT_GE(fallback, 0) << "the learned-FP note is no longer exported";
    const qsizetype guard = sarif.lastIndexOf(QStringLiteral("if (reason.isEmpty()"), fallback);
    ASSERT_GE(guard, 0);
    EXPECT_TRUE(sarif.mid(guard, fallback - guard).contains(QStringLiteral("FALSE_POSITIVE")))
        << "the fallback does not check the verdict";

    const qsizetype fp = sarif.indexOf(QStringLiteral("partialFp[\"primaryLocationLineHash/v1\"]"));
    ASSERT_GE(fp, 0) << "the partial fingerprint not found";
    const QString assign = sarif.mid(fp, sarif.indexOf(QChar(';'), fp) - fp);
    EXPECT_FALSE(assign.contains(QStringLiteral("dedupKey")))
        << "the fingerprint holds the line number, so alerts re-key when lines move";
    EXPECT_TRUE(assign.contains(QStringLiteral("computeFingerprint(")));
}

// INV-7 — row 302
TEST(AuditDialogReviewFixes, ProjectTextIsEscapedInRichText) {
    const QString src = auditSource();
    EXPECT_TRUE(src.contains(QStringLiteral("\"<b>Project:</b> \" + m_projectPath.toHtmlEscaped()")))
        << "the project path goes into a rich-text label unescaped";
    EXPECT_TRUE(src.contains(QStringLiteral("prev.timestamp.left(10).toHtmlEscaped()")))
        << "trend.json's timestamp goes into rich text unescaped";
}

// INV-8 — row 303
TEST(AuditDialogReviewFixes, StuckProcessIsReplacedBeforeTheNextCheck) {
    const QString body = functionText(auditSource(), "void AuditDialog::runNextCheck() {");
    ASSERT_FALSE(body.isEmpty()) << "runNextCheck not found";
    const qsizetype start = body.indexOf(QStringLiteral("m_process->start("));
    ASSERT_GE(start, 0) << "the check's process start not found";
    const QString before = body.left(start);
    const qsizetype probe = before.indexOf(QStringLiteral("m_process->state() != QProcess::NotRunning"));
    ASSERT_GE(probe, 0) << "a process that outlived SIGKILL is reused: start() fails, "
                           "waitForStarted() says true, and every later check times out";
    const QString replace = before.mid(probe);
    EXPECT_TRUE(replace.contains(QStringLiteral("releaseProcess(m_process)")));
    EXPECT_TRUE(replace.contains(QStringLiteral("m_process = new QProcess(this)")));
    EXPECT_TRUE(replace.contains(QStringLiteral("connectProcessSignals()")));
}

// INV-9 — row 305
TEST(AuditDialogReviewFixes, PercentInANameIsNotFilledIn) {
    ants_test::XdgGuard env;
    QTemporaryDir project;
    ASSERT_TRUE(project.isValid());
    Config cfg;
    AuditDialog dlg(project.path(), nullptr, &cfg);
    CheckResult r;
    r.checkId = QStringLiteral("x");
    r.checkName = QStringLiteral("rule %6");
    Finding f;
    f.checkId = r.checkId;
    f.file = QStringLiteral("src/a%1.cpp");
    f.line = 7;
    f.message = QStringLiteral("the message");
    r.findings = {f};
    r.findingCount = 1;
    const QString text = dlg.plainTextResultsForTest({r});
    EXPECT_TRUE(text.contains(QStringLiteral("src/a%1.cpp:7  the message")))
        << "a %1 in the file name took the message";
    EXPECT_TRUE(text.contains(QStringLiteral("rule %6")))
        << "a %6 in the check name took the finding count";

    const QString row = functionText(auditSource(),
        "QString AuditDialog::findingRowHtml(const Finding &f) const {");
    ASSERT_FALSE(row.isEmpty()) << "findingRowHtml not found";
    EXPECT_FALSE(row.contains(QStringLiteral(".arg(f.file.toHtmlEscaped()).arg(f.line)")))
        << "a %2 in the file name takes the line number in the results pane";
}

// INV-10 and INV-11 — rows 306 and 1322
TEST(AuditDialogReviewFixes, BatchTriageTakesOnlyItsOwnKeysAndSaysWhenOff) {
    const QString body = functionText(auditSource(),
        "void AuditDialog::requestAiTriageBatch(const QStringList &dedupKeys) {");
    ASSERT_FALSE(body.isEmpty()) << "requestAiTriageBatch not found";
    EXPECT_TRUE(body.contains(QStringLiteral("if (!sentKeys.contains(key)) continue;")))
        << "a verdict is applied to any key the model returns";
    EXPECT_FALSE(body.contains(QStringLiteral("if (key.isEmpty()) continue;")));

    const QString off = between(body,
        QStringLiteral("if (!cfg.aiEnabled() || endpoint.isEmpty())"),
        QStringLiteral("return;"));
    ASSERT_FALSE(off.isEmpty()) << "the AI-off check not found";
    EXPECT_TRUE(off.contains(QStringLiteral("setFullText(")))
        << "a queued batch with AI turned off returns with no status line";
}

// INV-12 — ANTS-6982
TEST(AuditDialogReviewFixes, FilteredViewShowsNoTrendLine) {
    const QString trend = functionText(auditSource(),
        "QString AuditDialog::trendLineHtml(const SummaryCounts &c) const {");
    ASSERT_FALSE(trend.isEmpty()) << "trendLineHtml not found";
    const qsizetype ret = trend.indexOf(QStringLiteral("return QString();"));
    ASSERT_GE(ret, 0) << "trendLineHtml has no early return";
    const QString gate = trend.left(ret);
    for (const char *flag : {"m_showNewOnly", "m_sinceBaseline", "m_recentOnly"})
        EXPECT_TRUE(gate.contains(QLatin1String(flag)))
            << "the trend line compares a filtered count with a full run's; "
               "its early return lacks " << flag;
}

// INV-13 — ANTS-6982
TEST(AuditDialogReviewFixes, TodoScanKeepsCommentLines) {
    const QString reg = between(auditSource(),
        QStringLiteral("addGrepCheck(\"todo_scan\""), QStringLiteral("addFindCheck("));
    ASSERT_FALSE(reg.isEmpty()) << "todo_scan not found";
    EXPECT_TRUE(reg.contains(QStringLiteral("m_checks.last().codeLinesOnly = false;")))
        << "todo_scan drops every annotation written in a comment";
}

// INV-14 — ANTS-6982, row 295's race on the Defer all path
TEST(AuditDialogReviewFixes, DeferAllFilesWhatItPrompted) {
    const QString tab = functionText(auditSource(), "void AuditDialog::buildDebtSweepTab() {");
    ASSERT_FALSE(tab.isEmpty()) << "buildDebtSweepTab not found";
    const QString handler = between(tab, QStringLiteral("connect(m_debtDeferAllBtn"),
                                    QStringLiteral("btnRow->addWidget(m_debtDeferAllBtn);"));
    ASSERT_FALSE(handler.isEmpty()) << "the Defer all handler not found";
    const qsizetype prompt = handler.indexOf(QStringLiteral("QInputDialog::getText("));
    ASSERT_GE(prompt, 0) << "the heading prompt not found";
    const QString after = handler.mid(prompt);
    EXPECT_FALSE(after.contains(QStringLiteral("debtDeferToRoadmap(m_debtFindings")))
        << "Defer all files whatever list a re-scan put there during the prompt";
    EXPECT_FALSE(after.contains(QStringLiteral("m_debtFindings.clear()")))
        << "Defer all clears findings a re-scan added during the prompt";
    EXPECT_TRUE(after.contains(QStringLiteral("dropDebtFinding(m_debtFindings, ")))
        << "Defer all does not drop what it filed by content";
}

// INV-15 — ANTS-6982
TEST(AuditDialogReviewFixes, EarlierRunsBlameIsDropped) {
    const QString src = auditSource();
    const QString run = functionText(src, "void AuditDialog::runAudit() {");
    ASSERT_FALSE(run.isEmpty()) << "runAudit not found";
    ASSERT_TRUE(run.contains(QStringLiteral("m_blameCache.clear();")));
    EXPECT_TRUE(run.contains(QStringLiteral("++m_blameGeneration;")))
        << "a new run does not mark the blame jobs still running as stale";
    const QString blame = functionText(src, "void AuditDialog::startQueuedBlame() {");
    ASSERT_FALSE(blame.isEmpty()) << "startQueuedBlame not found";
    const qsizetype insert = blame.indexOf(QStringLiteral("m_blameCache.insert("));
    ASSERT_GE(insert, 0) << "the blame result is not cached";
    const qsizetype check = blame.indexOf(QStringLiteral("generation == m_blameGeneration"));
    ASSERT_GE(check, 0) << "a blame job from the last run refills the cleared cache";
    EXPECT_LT(check, insert);
}
