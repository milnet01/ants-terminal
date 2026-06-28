// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "indiereviewdialog.h"

#include "briefdispatch.h"
#include "config.h"

#include <QDate>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString indieReviewSystemPrompt() {
    return QStringLiteral(
        "You are an independent code reviewer. You receive one subsystem's "
        "source files (inlined verbatim and fenced — you have no Read tool) "
        "plus the project's coding / testing / documentation standards. "
        "Review for correctness, security, and conformance to those "
        "standards. Report each issue on its own line as "
        "`- [SEV] file:line — description` where SEV is one of "
        "CRITICAL/HIGH/MED/LOW. Do not invent file:line citations. If a file "
        "is clean, say nothing about it.");
}

QString synthSystemPrompt() {
    return QStringLiteral(
        "You are a code-review synthesiser. From the per-subsystem reports "
        "below, write a concise cross-cutting summary of themes that span "
        "more than one subsystem, grouped by severity. Do not repeat "
        "single-subsystem findings verbatim.");
}

}  // namespace

IndieReviewDialog::IndieReviewDialog(QString projectCwd, QWidget *parent,
                                     Config *config)
    : ReviewDialogBase(std::move(projectCwd), parent, config) {
    setWindowTitle(tr("Independent Code Review"));
    buildControls();
    runPartition();   // derivePartition() → setLanes() + rebuildPartitionPanel()
}

void IndieReviewDialog::buildControls() {
    auto *panel = new QWidget(this);
    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 0, 0, 0);

    // Lane checklist (deselect before dispatch).
    lay->addWidget(new QLabel(tr("Lanes to review (uncheck to skip):"), panel));
    m_laneList = new QListWidget(panel);
    connect(m_laneList, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *item) {
                if (!item) return;
                setLaneSelected(item->data(Qt::UserRole).toString(),
                                item->checkState() == Qt::Checked);
            });
    lay->addWidget(m_laneList, 1);

    // Re-review (loop) button.
    auto *reReview = new QPushButton(tr("Re-review lanes with findings"), panel);
    connect(reReview, &QPushButton::clicked, this,
            &IndieReviewDialog::onReReviewClicked);
    lay->addWidget(reReview);

    // Results view.
    m_resultsView = new QPlainTextEdit(panel);
    m_resultsView->setReadOnly(true);
    m_resultsView->setPlaceholderText(
        tr("Corroborated findings appear here after dispatch."));
    lay->addWidget(m_resultsView, 1);

    // Fold-in mode.
    auto *foldBox = new QGroupBox(tr("Fold-in mode"), panel);
    auto *foldLay = new QVBoxLayout(foldBox);
    m_perFindingRadio = new QRadioButton(tr("Per-finding (allocate ROADMAP IDs)"),
                                         foldBox);
    auto *narrativeRadio =
        new QRadioButton(tr("Narrative (free-text prose, no IDs)"), foldBox);
    m_perFindingRadio->setChecked(true);
    connect(m_perFindingRadio, &QRadioButton::toggled, this, [this](bool on) {
        setFoldInMode(on ? FoldInMode::PerFinding : FoldInMode::Narrative);
    });
    foldLay->addWidget(m_perFindingRadio);
    foldLay->addWidget(narrativeRadio);
    m_narrativeEdit = new QPlainTextEdit(foldBox);
    m_narrativeEdit->setPlaceholderText(
        tr("Narrative fold-in text (Closed-inline / Deferred / "
           "False-positives)…"));
    connect(m_narrativeEdit, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_narrativeEdit) setNarrativeText(m_narrativeEdit->toPlainText());
    });
    // Per-finding is default → narrative editor starts hidden so the fold-in
    // box doesn't reserve dead vertical space (mirrors cold-eyes).
    m_narrativeEdit->setVisible(narrativeRadio->isChecked());
    connect(narrativeRadio, &QRadioButton::toggled, m_narrativeEdit,
            &QWidget::setVisible);
    foldLay->addWidget(m_narrativeEdit);
    lay->addWidget(foldBox);

    setResultsWidget(panel);
}

QList<ReviewLane> IndieReviewDialog::derivePartition() {
    m_allLanes = IndieReviewEngine::derivePartition(projectCwd());
    rebuildPartitionPanel();
    return selectedReviewLanes();
}

QList<ReviewLane> IndieReviewDialog::selectedReviewLanes() const {
    QList<ReviewLane> out;
    for (const IndieReviewEngine::Lane &lane : m_allLanes) {
        if (m_deselected.contains(lane.name)) continue;
        const QString title = QStringLiteral("%1 (%2 files)")
                                  .arg(lane.name)
                                  .arg(lane.sourcePaths.size());
        out << ReviewLane{ lane.name, title, lane.summary };
    }
    return out;
}

void IndieReviewDialog::setLaneSelected(const QString &laneName, bool on) {
    if (on) m_deselected.remove(laneName);
    else    m_deselected.insert(laneName);
    setLanes(selectedReviewLanes());   // refresh base lane tabs
}

bool IndieReviewDialog::isLaneSelected(const QString &laneName) const {
    return !m_deselected.contains(laneName);
}

const IndieReviewEngine::Lane *
IndieReviewDialog::laneByName(const QString &name) const {
    for (const IndieReviewEngine::Lane &lane : m_allLanes)
        if (lane.name == name) return &lane;
    return nullptr;
}

QString IndieReviewDialog::assembleCappedPrompt(const QString &brief) const {
    const QByteArray utf = brief.toUtf8();
    if (utf.size() <= kPromptCapBytes) return brief;
    // assembleBriefForDispatch is uncapped (inlines whole source + standards
    // bodies). Backstop a runaway lane at the budget; the fence preamble
    // already frames the bodies as data, so a clipped tail stays safe.
    const QString marker = QStringLiteral(
        "\n[brief truncated — lane exceeds 200 KiB budget]\n");
    // ANTS-2205 — shared clip-to-cap + fence-close tail (was ANTS-1991 inline).
    return BriefDispatch::clipToCapClosingFence(utf, marker, kPromptCapBytes);
}

LlmRequest IndieReviewDialog::composeBrief(const ReviewLane &laneRef) {
    LlmRequest req;
    if (config()) {
        req.endpoint = config()->aiEndpoint();
        req.apiKey   = config()->aiApiKey();
        req.model    = config()->aiModel();
    }
    req.maxTokens    = 2048;
    req.systemPrompt = indieReviewSystemPrompt();

    const IndieReviewEngine::Lane *lane = laneByName(laneRef.id);
    if (!lane) {
        req.userPrompt = QStringLiteral("[indie-review: lane \"%1\" not found]")
                             .arg(laneRef.id);
        return req;
    }
    // Reuse the engine's purpose-built dispatch brief: it already fences
    // source bodies, inlines the standards docs, injects the ants::falsepos
    // prior-FP block for ("indie-review", lane.name), and appends the
    // ROADMAP slice. Do NOT re-add any of that — just sum-cap (INV-2/INV-3).
    const QString brief =
        IndieReviewEngine::assembleBriefForDispatch(projectCwd(), *lane);
    req.userPrompt = assembleCappedPrompt(brief);
    return req;
}

void IndieReviewDialog::onAllReportsCollected(
    const QHash<QString, QString> &reportsById) {
    m_lastReports = reportsById;
    m_results = Results{};
    m_results.corroborated =
        IndieReviewEngine::corroboratedFindings(projectCwd(), reportsById, 2);

    const auto all =
        IndieReviewEngine::corroboratedFindings(projectCwd(), reportsById, 1);
    QSet<QString> corrKeys;
    for (const auto &f : m_results.corroborated)
        corrKeys.insert(f.file + QChar(':') + QString::number(f.line));
    for (const auto &f : all) {
        const QString k = f.file + QChar(':') + QString::number(f.line);
        if (!corrKeys.contains(k)) m_results.uncorroborated << f;
    }

    renderResultsWidget();
    maybeDispatchSynthesis();
}

void IndieReviewDialog::maybeDispatchSynthesis() {
    if (m_lastReports.isEmpty()) return;
    if (!config() || !endpointDispatchable(config()->aiEndpoint())) return;
    dispatchSynthesis();
}

void IndieReviewDialog::dispatchSynthesis() {
    const QString extras =
        IndieReviewEngine::assembleThreatModelExtras(projectCwd());
    LlmJob job;
    job.id = QStringLiteral("synthesis");
    if (config()) {
        job.request.endpoint = config()->aiEndpoint();
        job.request.apiKey   = config()->aiApiKey();
        job.request.model    = config()->aiModel();
    }
    job.request.maxTokens    = 2048;
    job.request.systemPrompt = synthSystemPrompt();
    job.request.userPrompt   =
        IndieReviewEngine::synthesisPrompt(m_lastReports, extras);
    dispatchOne(job, [this](const LlmResult &r) {
        m_synthText = r.text;
        renderResultsWidget();
    });
}

QStringList IndieReviewDialog::lanesToReReview() const {
    QSet<QString> s;
    const auto collect =
        [&](const QList<IndieReviewEngine::CorroboratedFinding> &fs) {
            for (const auto &f : fs)
                for (const QString &l : f.citingLanes) s.insert(l);
        };
    collect(m_results.corroborated);
    collect(m_results.uncorroborated);
    QStringList out(s.begin(), s.end());
    out.sort();
    return out;
}

void IndieReviewDialog::onReReviewClicked() {
    const QStringList lanes = lanesToReReview();
    if (lanes.isEmpty()) {
        if (statusLabel())
            statusLabel()->setText(tr("No findings to re-review."));
        return;
    }
    redispatch(lanes);
}

void IndieReviewDialog::performFoldIn() {
    const QString heading = activeReleaseHeading();
    const QString dateIso = QDate::currentDate().toString(Qt::ISODate);

    if (m_foldInMode == FoldInMode::Narrative) {
        QString block = QStringLiteral("### 🔍 Indie-review %1\n\n").arg(dateIso);
        const QString body = m_narrative.trimmed();
        block += body.isEmpty() ? QStringLiteral("- (no narrative provided)")
                                : body;
        if (!block.endsWith(QChar('\n'))) block += QChar('\n');
        insertFoldInBlock(heading, block);
        return;
    }

    const auto &actionable = m_results.corroborated;
    if (actionable.isEmpty()) {
        if (statusLabel())
            statusLabel()->setText(
                tr("No corroborated findings to fold in."));
        return;
    }
    const QList<int> ids = allocateFoldInIds(actionable.size());
    if (ids.isEmpty()) return;   // base surfaced the reason
    const QString block = IndieReviewEngine::templateIndieReviewFoldInBlock(
        actionable, ids, dateIso);
    insertFoldInBlock(heading, block);
}

void IndieReviewDialog::rebuildPartitionPanel() {
    if (!m_laneList) return;
    QSignalBlocker block(m_laneList);   // don't fire itemChanged during rebuild
    m_laneList->clear();
    for (const IndieReviewEngine::Lane &lane : m_allLanes) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 (%2 files)")
                .arg(lane.name)
                .arg(lane.sourcePaths.size()),
            m_laneList);
        item->setData(Qt::UserRole, lane.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_deselected.contains(lane.name) ? Qt::Unchecked
                                                            : Qt::Checked);
    }
}

void IndieReviewDialog::renderResultsWidget() {
    if (!m_resultsView) return;
    QString s;
    s += tr("Corroborated findings (≥2 lanes): %1\n")
             .arg(m_results.corroborated.size());
    for (const auto &f : m_results.corroborated)
        s += QStringLiteral("  • %1:%2  [%3]\n")
                 .arg(f.file)
                 .arg(f.line)
                 .arg(f.citingLanes.join(QStringLiteral(", ")));
    s += tr("\nUncorroborated (single-lane): %1\n")
             .arg(m_results.uncorroborated.size());
    for (const auto &f : m_results.uncorroborated)
        s += QStringLiteral("  • %1:%2  [%3]\n")
                 .arg(f.file)
                 .arg(f.line)
                 .arg(f.citingLanes.join(QStringLiteral(", ")));
    if (!m_synthText.isEmpty())
        s += QStringLiteral("\n=== Cross-cutting synthesis ===\n\n")
             + m_synthText;
    m_resultsView->setPlainText(s);
}
