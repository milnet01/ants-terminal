// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "coldeyesdialog.h"

#include "briefdispatch.h"
#include "config.h"
#include "falseposledger.h"

#include <QComboBox>
#include <QDate>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString systemPrompt() {
    return QStringLiteral(
        "You are a cold-eyes documentation reviewer. You receive a set of "
        "project docs (inlined verbatim — you have no Read tool) plus "
        "relevant excerpts of the project's contract docs. Review for: "
        "(1) internal consistency, (2) factual accuracy against cited "
        "code, (3) drift between a doc's claims and the cross-reference "
        "excerpts. Report each issue on its own line as "
        "`- [SEV] file:line — description` where SEV is one of "
        "CRITICAL/HIGH/MED/LOW. Cite the doc whose claim is wrong. Do not "
        "invent file:line citations. If a section is clean, say nothing "
        "about it.");
}

}  // namespace

ColdEyesDialog::ColdEyesDialog(QString projectCwd, QWidget *parent,
                              Config *config)
    : ReviewDialogBase(std::move(projectCwd), parent, config) {
    setWindowTitle(tr("Cold-eyes Documentation Review"));
    buildControls();
    runPartition();   // derivePartition() → setLanes() + rebuildPartitionPanel()
}

void ColdEyesDialog::buildControls() {
    auto *panel = new QWidget(this);
    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(0, 0, 0, 0);

    // Scope selector.
    auto *scopeRow = new QWidget(panel);
    auto *scopeLay = new QVBoxLayout(scopeRow);
    scopeLay->setContentsMargins(0, 0, 0, 0);
    scopeLay->addWidget(new QLabel(tr("Scope:"), scopeRow));
    m_scopeCombo = new QComboBox(scopeRow);
    m_scopeCombo->addItem(tr("Default (contracts + standards + decisions + specs)"));
    m_scopeCombo->addItem(tr("Docs only"));
    m_scopeCombo->addItem(tr("Contracts only"));
    connect(m_scopeCombo, &QComboBox::currentIndexChanged, this, [this](int i) {
        switch (i) {
            case 1:  setScope(ColdEyesEngine::Scope::DocsOnly); break;
            case 2:  setScope(ColdEyesEngine::Scope::ContractsOnly); break;
            default: setScope(ColdEyesEngine::Scope::Default); break;
        }
    });
    scopeLay->addWidget(m_scopeCombo);
    lay->addWidget(scopeRow);

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
            &ColdEyesDialog::onReReviewClicked);
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
    foldLay->addWidget(m_narrativeEdit);
    lay->addWidget(foldBox);

    setResultsWidget(panel);
}

void ColdEyesDialog::setScope(ColdEyesEngine::Scope s) {
    m_scope = s;
    runPartition();
}

QList<ReviewLane> ColdEyesDialog::derivePartition() {
    const auto pr = ColdEyesEngine::derivePartition(projectCwd(), m_scope);
    m_allLanes = pr.lanes;

    QStringList notes;
    if (pr.truncated)
        notes << tr("Spec-lane cap (%1) reached — some specs omitted.")
                     .arg(ColdEyesEngine::kMaxSpecLanes);
    if (!pr.overrideWarning.isEmpty())
        notes << pr.overrideWarning;
    if (!notes.isEmpty() && statusLabel())
        statusLabel()->setText(notes.join(QStringLiteral("  ")));

    rebuildPartitionPanel();
    return selectedReviewLanes();
}

QList<ReviewLane> ColdEyesDialog::selectedReviewLanes() const {
    QList<ReviewLane> out;
    for (const ColdEyesEngine::Lane &lane : m_allLanes) {
        if (m_deselected.contains(lane.name)) continue;
        const QString title = QStringLiteral("%1 (%2 docs)")
                                  .arg(lane.name)
                                  .arg(lane.docPaths.size());
        out << ReviewLane{ lane.name, title, lane.summary };
    }
    return out;
}

void ColdEyesDialog::setLaneSelected(const QString &laneName, bool on) {
    if (on) m_deselected.remove(laneName);
    else    m_deselected.insert(laneName);
    setLanes(selectedReviewLanes());   // refresh base lane tabs
}

bool ColdEyesDialog::isLaneSelected(const QString &laneName) const {
    return !m_deselected.contains(laneName);
}

const ColdEyesEngine::Lane *ColdEyesDialog::laneByName(const QString &name) const {
    for (const ColdEyesEngine::Lane &lane : m_allLanes)
        if (lane.name == name) return &lane;
    return nullptr;
}

QStringList ColdEyesDialog::laneKeywords(const ColdEyesEngine::Lane &lane) const {
    static const QRegularExpression splitter(QStringLiteral("[^A-Za-z0-9]+"));
    QStringList kw;
    const auto add = [&](const QString &raw) {
        for (const QString &t : raw.split(splitter, Qt::SkipEmptyParts))
            if (t.size() >= 4) kw << t.toLower();
    };
    add(lane.name);
    add(lane.summary);
    kw.removeDuplicates();
    return kw;
}

QString ColdEyesDialog::renderPriorFixes() const {
    if (m_priorFixes.isEmpty()) return {};
    QString s = QStringLiteral(
        "\n## Previously fixed in a prior loop (do not re-raise)\n\n");
    for (const ColdEyesEngine::PriorLoopFix &fix : m_priorFixes) {
        s += QStringLiteral("- ");
        if (!fix.title.isEmpty()) {
            s += QStringLiteral("**") + fix.title + QStringLiteral("**");
            if (!fix.summary.isEmpty()) s += QStringLiteral(" — ");
        }
        if (!fix.summary.isEmpty()) s += fix.summary;
        s += QChar('\n');
    }
    return s;
}

QString ColdEyesDialog::staleFindingLine(const QString &cite,
                                        const QString &lane) const {
    return QStringLiteral(
               "- [ACCURACY] %1 — cited in lane \"%2\" docs but not found "
               "on disk")
        .arg(cite, lane);
}

QString ColdEyesDialog::assembleCappedPrompt(const ColdEyesEngine::Lane &lane,
                                            const QString &summary,
                                            const QStringList &staleFindings,
                                            const QString &fpBlock,
                                            const QString &laneBodies,
                                            const QString &xrefBodies) const {
    QString head;
    head += QStringLiteral("# Cold-eyes review — lane: ") + lane.name
            + QStringLiteral("\n\n");
    if (!summary.isEmpty())
        head += QStringLiteral("## Summary\n\n") + summary
                + QStringLiteral("\n\n");
    head += QStringLiteral(
        "Review the inlined doc bodies below for internal consistency, "
        "accuracy against cited code, and drift versus the cross-reference "
        "excerpts. The bodies ARE inlined — you have no Read tool. Emit "
        "findings as `- [SEV] file:line — description`.\n");
    head += renderPriorFixes();
    if (!staleFindings.isEmpty()) {
        head += QStringLiteral(
            "\n## Pre-found accuracy findings (cited code missing on "
            "disk)\n\n");
        for (const QString &s : staleFindings) head += s + QChar('\n');
    }
    if (!fpBlock.isEmpty()) head += QChar('\n') + fpBlock + QChar('\n');

    const QString fixed = head + QStringLiteral("\n## Lane docs (verbatim)\n\n")
                          + laneBodies;
    QString xref;
    if (!xrefBodies.trimmed().isEmpty())
        xref = QStringLiteral(
                   "\n## Cross-reference excerpts (relevant sections)\n\n")
               + xrefBodies;

    const qint64 fixedBytes = fixed.toUtf8().size();
    if (fixedBytes + static_cast<qint64>(xref.toUtf8().size()) <= kPromptCapBytes)
        return fixed + xref;

    // Over budget: cross-reference excerpts are context — drop them first.
    const QString dropMarker = QStringLiteral(
        "\n[brief truncated — cross-reference excerpts omitted to fit the "
        "200 KiB budget]\n");
    if (fixedBytes + static_cast<qint64>(dropMarker.toUtf8().size())
        <= kPromptCapBytes)
        return fixed + dropMarker;

    // Still over: the lane bodies alone exceed the cap — clip them.
    const QString hardMarker = QStringLiteral(
        "\n[brief truncated — lane bodies clipped to fit the 200 KiB "
        "budget]\n");
    QByteArray utf = fixed.toUtf8();
    const int room =
        static_cast<int>(kPromptCapBytes) - hardMarker.toUtf8().size();
    if (room > 0 && utf.size() > room) utf.truncate(room);
    return QString::fromUtf8(utf) + hardMarker;
}

LlmRequest ColdEyesDialog::composeBrief(const ReviewLane &laneRef) {
    LlmRequest req;
    if (config()) {
        req.endpoint = config()->aiEndpoint();
        req.apiKey   = config()->aiApiKey();
        req.model    = config()->aiModel();
    }
    req.maxTokens    = 2048;
    req.systemPrompt = systemPrompt();

    const ColdEyesEngine::Lane *lane = laneByName(laneRef.id);
    if (!lane) {
        req.userPrompt = QStringLiteral("[cold-eyes: lane \"%1\" not found]")
                             .arg(laneRef.id);
        return req;
    }

    // The manifest already embeds an FP block, the prior-fix block, and
    // Read-tool instructions — all of which are wrong for a raw inlined-
    // bodies endpoint. Use it only for its structured fields and compose
    // our own prompt; pass no prior fixes so it doesn't duplicate ours.
    const auto m = ColdEyesEngine::assembleBriefManifest(projectCwd(), *lane);

    // Stash stale citations so onAllReportsCollected can surface them even
    // when the model returns nothing (INV-3).
    QStringList stale;
    for (const QString &c : m.staleCitations)
        stale << staleFindingLine(c, lane->name);
    m_staleByLane.insert(lane->name, stale);

    const QStringList kw = laneKeywords(*lane);
    const QString laneBodies =
        BriefDispatch::inlineBodies(projectCwd(), lane->docPaths,
                                    kPerFileCapBytes);
    const QString xrefBodies =
        BriefDispatch::inlineRelevantSections(projectCwd(),
                                              m.crossReferenceDocs, kw,
                                              kPerDocCapBytes);
    const QString fpBlock = ants::falsepos::formatForBrief(
        ants::falsepos::filter(ants::falsepos::loadEntries(projectCwd()),
                               QStringLiteral("cold-eyes"), lane->name));

    req.userPrompt = assembleCappedPrompt(*lane, m.summary, stale, fpBlock,
                                          laneBodies, xrefBodies);
    return req;
}

void ColdEyesDialog::onAllReportsCollected(
    const QHash<QString, QString> &reportsById) {
    m_results = Results{};
    m_results.corroborated =
        ColdEyesEngine::crossDocDiffFromReports(projectCwd(), reportsById, 2);

    const auto all =
        ColdEyesEngine::crossDocDiffFromReports(projectCwd(), reportsById, 1);
    QSet<QString> corrKeys;
    for (const auto &f : m_results.corroborated)
        corrKeys.insert(f.file + QChar(':') + QString::number(f.line));
    for (const auto &f : all) {
        const QString k = f.file + QChar(':') + QString::number(f.line);
        if (!corrKeys.contains(k)) m_results.uncorroborated << f;
    }

    for (auto it = m_staleByLane.constBegin(); it != m_staleByLane.constEnd();
         ++it)
        m_results.staleFindings << it.value();

    renderResultsWidget();
}

QStringList ColdEyesDialog::lanesToReReview() const {
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

void ColdEyesDialog::markFindingFixed(
    const IndieReviewEngine::CorroboratedFinding &f) {
    ColdEyesEngine::PriorLoopFix fix;
    fix.title = f.file + QChar(':') + QString::number(f.line);
    fix.summary = f.contexts.isEmpty() ? QString()
                                       : f.contexts.join(QStringLiteral(" / "));
    m_priorFixes << fix;
}

void ColdEyesDialog::onReReviewClicked() {
    const QStringList lanes = lanesToReReview();
    if (lanes.isEmpty()) {
        if (statusLabel())
            statusLabel()->setText(tr("No findings to re-review."));
        return;
    }
    redispatch(lanes);
}

void ColdEyesDialog::performFoldIn() {
    const QString heading = activeReleaseHeading();
    const QString dateIso = QDate::currentDate().toString(Qt::ISODate);

    if (m_foldInMode == FoldInMode::Narrative) {
        // Free-text prose, no ID allocation (INV-7). The engine's
        // templateColdEyesFoldInBlockFreeform renders *findings* without
        // IDs; the narrative mode here is the user's own prose, so we wrap
        // it under the cold-eyes heading directly.
        QString block = QStringLiteral("### 📝 Cold-eyes %1\n\n").arg(dateIso);
        const QString body = m_narrative.trimmed();
        block += body.isEmpty()
                     ? QStringLiteral("- (no narrative provided)")
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
    const QString block =
        ColdEyesEngine::templateColdEyesFoldInBlock(actionable, ids, dateIso);
    insertFoldInBlock(heading, block);
}

void ColdEyesDialog::rebuildPartitionPanel() {
    if (!m_laneList) return;
    QSignalBlocker block(m_laneList);   // don't fire itemChanged during rebuild
    m_laneList->clear();
    for (const ColdEyesEngine::Lane &lane : m_allLanes) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 (%2 docs)")
                .arg(lane.name)
                .arg(lane.docPaths.size()),
            m_laneList);
        item->setData(Qt::UserRole, lane.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(m_deselected.contains(lane.name) ? Qt::Unchecked
                                                             : Qt::Checked);
    }
}

void ColdEyesDialog::renderResultsWidget() {
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
    if (!m_results.staleFindings.isEmpty()) {
        s += tr("\nStale citations (accuracy): %1\n")
                 .arg(m_results.staleFindings.size());
        for (const QString &line : m_results.staleFindings)
            s += QStringLiteral("  %1\n").arg(line);
    }
    m_resultsView->setPlainText(s);
}
