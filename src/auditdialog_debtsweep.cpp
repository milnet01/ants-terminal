// ANTS-1677 auditdialog piece 3/5 — the Debt Sweep tab
#include "auditdialog.h"
#include "auditdialog_internal.h"
#include "auditautofix.h"
#include "auditfpledger.h"
#include "roadmapfoldin.h"
#include "debtsweepengine.h"
#include "llmclient.h"
#include "dialogchrome.h"
#include "tooldetectionengine.h"
#include "config.h"
#include <QDate>
#include <QFont>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QTextBrowser>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

using namespace auditdialogdetail;

Finding AuditDialog::debtToAuditFinding(const DebtSweepEngine::Finding &d) {
    Finding f;
    f.checkId   = d.detectorId;
    f.checkName = d.detectorId;
    f.file      = d.file;
    f.line      = d.line;
    f.message   = d.message;
    return f;
}

// ANTS-5057 — the sweep reads the whole tree, blames every file holding a
// TODO and runs the packaging script, so it runs on a worker; the allowlist
// filter and the render run here when it finishes. One scan at a time: a
// request made while one runs is served by one more scan afterwards.
void AuditDialog::requestDebtScan() {
    if (m_debtScanRunning) {
        m_debtScanAgain = true;
        return;
    }
    m_debtScanRunning = true;
    if (m_debtScanBtn) m_debtScanBtn->setEnabled(false);
    auto result = std::make_shared<QList<DebtSweepEngine::Finding>>();
    const QString root = m_projectPath;
    QThread *worker = QThread::create([root, result]() {
        DebtSweepEngine::ScanOptions opt;   // engine defaults
        *result = DebtSweepEngine::scanAll(root, opt);
    });
    // The thread deletes itself even if the dialog closes first; the result
    // connection below then simply never fires.
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, result]() {
        // Reuse the project allowlist so an Allowed debt finding stays hidden
        // across scans (the engine itself doesn't consult the allowlist).
        m_debtFindings.clear();
        for (const DebtSweepEngine::Finding &d : std::as_const(*result))
            if (!allowlisted(debtToAuditFinding(d)))
                m_debtFindings.append(d);
        m_debtScanned = true;
        m_debtScanRunning = false;
        if (m_debtScanBtn) m_debtScanBtn->setEnabled(true);
        renderDebtResults();
        if (m_debtStatus)
            m_debtStatus->setFullText(QString("Debt sweep — %1 finding%2")
                .arg(m_debtFindings.size())
                .arg(m_debtFindings.size() == 1 ? "" : "s"));
        if (std::exchange(m_debtScanAgain, false))
            requestDebtScan();
    }, Qt::QueuedConnection);
    worker->start();
}

bool AuditDialog::debtFixInline(const DebtSweepEngine::Finding &f) {
    return DebtSweepEngine::applyMechanicalFix(m_projectPath, f).applied;
}

bool AuditDialog::debtDeferToRoadmap(
    const QList<DebtSweepEngine::Finding> &deferred,
    const QString &releaseHeading) {
    if (deferred.isEmpty() || releaseHeading.isEmpty()) return false;
    QString needle = releaseHeading;
    while (needle.endsWith(QChar('\n')) || needle.endsWith(QChar('\r')))
        needle.chop(1);
    if (!roadmapHeadingExists(needle)) return false;   // don't burn IDs

    const QList<int> ids =
        RoadmapFoldIn::allocateIds(m_projectPath, deferred.size());
    if (ids.size() != deferred.size()) return false;

    // ANTS-3497 — stamp the project's own sniffed prefix (padded), not ANTS.
    const QString block = DebtSweepEngine::templateDebtSweepFoldInBlock(
        deferred, ids, QDate::currentDate().toString(Qt::ISODate),
        RoadmapFoldIn::sniffIdPrefix(m_projectPath));
    return RoadmapFoldIn::insertBlock(m_projectPath, needle, block);
}

bool AuditDialog::debtAllow(const DebtSweepEngine::Finding &f,
                            const QString &reason) {
    return appendAllowlistEntry(debtToAuditFinding(f), reason);
}

QString AuditDialog::debtTriagePrompt() const {
    QList<DebtSweepEngine::Finding> llm;
    for (const DebtSweepEngine::Finding &d : std::as_const(m_debtFindings))
        if (!d.autoFixable) llm.append(d);
    return DebtSweepEngine::triagePrompt(llm);
}

void AuditDialog::buildDebtSweepTab() {
    auto *tab = new QWidget(m_tabs);
    auto *col = new QVBoxLayout(tab);
    col->setSpacing(8);

    auto *intro = new QLabel(tab);
    intro->setText(
        "<b>Debt Sweep</b> — scans for code / test / doc / packaging drift "
        "accrued since the last tag. Fix mechanical items in place, defer the "
        "rest into ROADMAP, or allow a false positive.");
    intro->setWordWrap(true);
    col->addWidget(intro);

    auto *btnRow = new QHBoxLayout();
    m_debtScanBtn = new QPushButton(QStringLiteral("🧹 Scan for debt"), tab);
    m_debtScanBtn->setFixedHeight(32);
    connect(m_debtScanBtn, &QPushButton::clicked,
            this, &AuditDialog::onDebtScanClicked);
    btnRow->addWidget(m_debtScanBtn);

    m_debtDeferAllBtn =
        new QPushButton(QStringLiteral("📌 Defer all to ROADMAP"), tab);
    m_debtDeferAllBtn->setFixedHeight(32);
    m_debtDeferAllBtn->setEnabled(false);
    m_debtDeferAllBtn->setToolTip(
        "Fold every listed finding into ROADMAP.md as a dated debt-sweep block.");
    connect(m_debtDeferAllBtn, &QPushButton::clicked, this, [this]() {
        if (m_debtFindings.isEmpty()) return;
        // ANTS-3346 — a raw scan is a mix of real, FP-prone, and mechanical
        // findings; folding the whole lot into ROADMAP writes every one as a
        // tracked item (this is how 1106 false positives once landed there).
        // A human is present, so gate with a confirmation (not a hard refuse)
        // that points at Triage with AI. Shares the pure verdict with the MCP
        // verb; the total-count threshold naturally exempts a single [defer].
        const auto verdict = DebtSweepEngine::evaluateTriageGate(
            m_debtFindings, /*triaged=*/false);
        if (!verdict.allowed) {
            const auto reply = QMessageBox::warning(this,
                "Defer all to ROADMAP",
                QString("%1 findings would be written into ROADMAP.md as "
                        "tracked items — %2 of them are judgment-required "
                        "(not mechanical auto-fixes) and haven't been "
                        "triaged.\n\nDeferring the whole scan folds its false "
                        "positives in too. Consider \"🧠 Triage with AI\" "
                        "first to drop them.\n\nDefer all %1 anyway?")
                    .arg(verdict.total).arg(verdict.nonAutoFixable),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes) return;
        }
        QString heading = RoadmapFoldIn::findActiveReleaseHeading(m_projectPath);
        bool ok = false;
        heading = QInputDialog::getText(this, "Defer all to ROADMAP",
            QString("Fold all %1 finding%2 into ROADMAP.md after this heading "
                    "(edit if wrong):")
                .arg(m_debtFindings.size())
                .arg(m_debtFindings.size() == 1 ? "" : "s"),
            QLineEdit::Normal, heading, &ok);
        if (!ok || heading.trimmed().isEmpty()) return;
        if (debtDeferToRoadmap(m_debtFindings, heading.trimmed())) {
            if (m_debtStatus)
                m_debtStatus->setFullText("Deferred all findings into ROADMAP.md");
            m_debtFindings.clear();
            renderDebtResults();
        } else {
            QMessageBox::warning(this, "Defer to ROADMAP",
                "Could not insert the fold-in block — check the heading exists "
                "verbatim in ROADMAP.md.");
        }
    });
    btnRow->addWidget(m_debtDeferAllBtn);

    m_debtTriageBtn = new QPushButton(QStringLiteral("🧠 Triage with AI"), tab);
    m_debtTriageBtn->setFixedHeight(32);
    m_debtTriageBtn->setEnabled(false);
    m_debtTriageBtn->setToolTip(
        "Send the non-mechanical findings to the configured AI endpoint for "
        "KEEP / DROP / DEFER triage. Requires Settings → AI → enabled.");
    connect(m_debtTriageBtn, &QPushButton::clicked,
            this, &AuditDialog::onDebtTriageClicked);
    btnRow->addWidget(m_debtTriageBtn);
    btnRow->addStretch();
    col->addLayout(btnRow);

    m_debtStatus = new ElidedLabel(tab);
    col->addWidget(m_debtStatus);

    m_debtResults = new QTextBrowser(tab);
    m_debtResults->setReadOnly(true);
    m_debtResults->setFont(QFont("monospace", 9));
    m_debtResults->setOpenLinks(false);
    m_debtResults->setOpenExternalLinks(false);
    connect(m_debtResults, &QTextBrowser::anchorClicked,
            this, &AuditDialog::onDebtAnchorClicked);
    col->addWidget(m_debtResults, 1);

    m_tabs->addTab(tab, tr("Debt Sweep"));
    renderDebtResults();   // initial "click Scan" placeholder
}

void AuditDialog::onDebtScanClicked() {
    if (m_debtStatus) m_debtStatus->setFullText("Scanning for debt…");
    requestDebtScan();   // ANTS-5057 — renders when the worker finishes
}

void AuditDialog::renderDebtResults() {
    if (!m_debtResults) return;
    m_debtResults->clear();

    const bool any = !m_debtFindings.isEmpty();
    if (m_debtDeferAllBtn) m_debtDeferAllBtn->setEnabled(any);

    bool hasLlmShaped = false;
    for (const DebtSweepEngine::Finding &d : std::as_const(m_debtFindings))
        if (!d.autoFixable) { hasLlmShaped = true; break; }
    if (m_debtTriageBtn) {
        Config cfg;
        m_debtTriageBtn->setEnabled(hasLlmShaped && cfg.aiEnabled()
                                    && !cfg.aiEndpoint().isEmpty());
    }

    if (!any) {
        m_debtResults->setHtml(m_debtScanned
            ? QStringLiteral("<i>No debt findings.</i>")
            : QStringLiteral("<i>Click \"Scan for debt\" to begin.</i>"));
        return;
    }

    static const QStringList catOrder = {
        "code_drift", "test_coverage", "doc_drift", "packaging_drift"};
    static const QHash<QString, QString> catLabel = {
        {"code_drift", "Code drift"}, {"test_coverage", "Test coverage"},
        {"doc_drift", "Doc drift"}, {"packaging_drift", "Packaging drift"}};

    QString html;
    for (const QString &cat : catOrder) {
        QStringList rows;
        for (int i = 0; i < m_debtFindings.size(); ++i) {
            const DebtSweepEngine::Finding &d = m_debtFindings.at(i);
            if (d.category != cat) continue;
            const QString loc = d.file.isEmpty()
                ? QStringLiteral("(project)")
                : (d.line > 0
                   ? QString("%1:%2").arg(d.file.toHtmlEscaped()).arg(d.line)
                   : d.file.toHtmlEscaped());
            QString actions;
            if (d.autoFixable)
                actions += QString(" <a href='ants-debt-fix://%1' "
                    "style='color:#4CAF50; text-decoration:none;'>[fix]</a>").arg(i);
            actions += QString(" <a href='ants-debt-defer://%1' "
                "style='color:#89B4FA; text-decoration:none;'>[defer]</a>").arg(i);
            actions += QString(" <a href='ants-debt-allow://%1' "
                "style='color:#FFA500; text-decoration:none;'>[allow]</a>").arg(i);
            rows << QString("<span style='color:#89B4FA;'>%1</span>  %2"
                            "  <span style='color:#666; font-size:9px;'>(%3)</span>%4")
                        .arg(loc, d.message.toHtmlEscaped(),
                             d.detectorId.toHtmlEscaped(), actions);
        }
        if (rows.isEmpty()) continue;
        html += QString("<div style='margin:6px 0 2px 0;'><b>%1</b> "
                        "<span style='color:#888;'>(%2)</span></div>")
                    .arg(catLabel.value(cat, cat)).arg(rows.size());
        html += "<div style='margin:0 0 8px 12px; white-space:pre-wrap;'>"
                + rows.join("<br>") + "</div>";
    }
    m_debtResults->setHtml(html);
}

void AuditDialog::onDebtAnchorClicked(const QUrl &url) {
    const QString scheme = url.scheme();
    bool okIdx = false;
    const QString raw = url.host().isEmpty() ? url.path().mid(1) : url.host();
    const int idx = raw.toInt(&okIdx);
    if (!okIdx || idx < 0 || idx >= m_debtFindings.size()) return;
    const DebtSweepEngine::Finding f = m_debtFindings.at(idx);

    if (scheme == "ants-debt-fix") {
        if (debtFixInline(f)) {
            if (m_debtStatus)
                m_debtStatus->setFullText(QString("Fixed: %1 (%2)")
                    .arg(f.file, f.detectorId));
            // ANTS-5057 — drop it now; the background re-scan confirms.
            m_debtFindings.removeAt(idx);
            requestDebtScan();
        } else if (m_debtStatus) {
            m_debtStatus->setFullText(
                "Fix did not apply (file changed under the scan?) — re-scan");
        }
        renderDebtResults();
        return;
    }

    if (scheme == "ants-debt-allow") {
        bool ok = false;
        const QString reason = QInputDialog::getText(this, "Allow debt finding",
            QString("Allow this finding (writes .audit_allowlist.json)?\n\n"
                    "Detector: %1\nLocation: %2\nMessage:  %3\n\nOptional reason:")
                .arg(f.detectorId,
                     f.file.isEmpty() ? QStringLiteral("(project)")
                                      : QString("%1:%2").arg(f.file, QString::number(f.line)),
                     f.message.left(200)),
            QLineEdit::Normal, QString(), &ok);
        if (!ok) return;
        if (debtAllow(f, reason)) {
            if (m_debtStatus)
                m_debtStatus->setFullText(QString("Allowlisted %1").arg(f.detectorId));
            // ANTS-5057 — drop it now; the background re-scan confirms.
            m_debtFindings.removeAt(idx);
            requestDebtScan();
        }
        renderDebtResults();
        return;
    }

    if (scheme == "ants-debt-defer") {
        QString heading = RoadmapFoldIn::findActiveReleaseHeading(m_projectPath);
        bool ok = false;
        heading = QInputDialog::getText(this, "Defer to ROADMAP",
            "Fold this finding into ROADMAP.md after this heading (edit if wrong):",
            QLineEdit::Normal, heading, &ok);
        if (!ok || heading.trimmed().isEmpty()) return;
        if (debtDeferToRoadmap({f}, heading.trimmed())) {
            m_debtFindings.removeAt(idx);
            if (m_debtStatus) m_debtStatus->setFullText("Deferred into ROADMAP.md");
            renderDebtResults();
        } else {
            QMessageBox::warning(this, "Defer to ROADMAP",
                "Could not insert — check the heading exists verbatim in "
                "ROADMAP.md.");
        }
        return;
    }
}

void AuditDialog::onDebtTriageClicked() {
    const QString prompt = debtTriagePrompt();
    if (prompt.isEmpty()) {
        QMessageBox::information(this, "Triage with AI",
            "No non-mechanical findings to triage.");
        return;
    }
    Config cfg;
    if (!cfg.aiEnabled() || cfg.aiEndpoint().isEmpty()) {
        QMessageBox::information(this, "Triage with AI",
            "Configure an AI endpoint in Settings → AI first.");
        return;
    }
    int llmCount = 0;
    for (const DebtSweepEngine::Finding &d : std::as_const(m_debtFindings))
        if (!d.autoFixable) ++llmCount;
    // ANTS-5010 — a keyless plain-http endpoint gets the findings unencrypted.
    const QString plaintextWarning =
        LlmClient::plaintextPromptWarning(cfg.aiEndpoint(), cfg.aiApiKey());
    const auto reply = QMessageBox::question(this, "Triage with AI",
        QString("Send %1 non-mechanical finding%2 to %3 for triage?")
            .arg(llmCount).arg(llmCount == 1 ? "" : "s")
            .arg(QUrl(cfg.aiEndpoint()).host().isEmpty()
                 ? cfg.aiEndpoint() : QUrl(cfg.aiEndpoint()).host())
            + (plaintextWarning.isEmpty()
                   ? QString() : QStringLiteral("\n\n") + plaintextWarning),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reply != QMessageBox::Yes) return;

    if (!m_debtLlm) {
        m_debtLlm = new LlmClient(this);
        connect(m_debtLlm, &LlmClient::finished, this,
                [this](const LlmResult &r) {
            if (m_debtTriageBtn) m_debtTriageBtn->setEnabled(true);
            if (!r.ok) {
                if (m_debtStatus)
                    m_debtStatus->setFullText("AI triage failed: " + r.error);
                return;
            }
            m_debtResults->append(
                "<hr><div style='margin:6px 0;'><b>🧠 AI triage</b></div>"
                "<div style='white-space:pre-wrap;'>"
                + r.text.toHtmlEscaped() + "</div>");
            if (m_debtStatus) m_debtStatus->setFullText("AI triage complete");
        });
    }
    LlmRequest req;
    req.endpoint   = cfg.aiEndpoint();
    req.apiKey     = cfg.aiApiKey();
    req.model      = cfg.aiModel();
    req.userPrompt = prompt;
    if (m_debtTriageBtn) m_debtTriageBtn->setEnabled(false);
    if (m_debtStatus) m_debtStatus->setFullText("AI triage in flight…");
    m_debtLlm->send(req);
}
