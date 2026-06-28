// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// ColdEyesDialog — native in-app documentation cold-eyes review
// (ANTS-1721). A ReviewDialogBase (ANTS-1727) subclass that wraps the
// pure-function ColdEyesEngine: it partitions the doc tree into lanes,
// composes a self-contained per-lane brief (doc bodies inlined, cross-
// reference contracts narrowed to relevant sections, prior false-positives
// and stale citations folded in), dispatches each lane to the configured
// LLM endpoint in parallel, corroborates the returned reports, and folds
// findings into ROADMAP.md — all without spending Claude orchestration
// tokens. The review→fix→re-review loop runs in-dialog and re-dispatches
// only the lanes that produced findings.

#pragma once

#include "indiereviewengine.h"  // CorroboratedFinding
#include "coldeyesengine.h"
#include "reviewdialogbase.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class QComboBox;
class QListWidget;
class QPlainTextEdit;
class QRadioButton;

class ColdEyesDialog : public ReviewDialogBase {
    Q_OBJECT
public:
    ColdEyesDialog(QString projectCwd, QWidget *parent, Config *config);

    enum class FoldInMode { PerFinding, Narrative };

    // Corroboration + stale-citation results, built in onAllReportsCollected.
    struct Results {
        QList<IndieReviewEngine::CorroboratedFinding> corroborated;
        QList<IndieReviewEngine::CorroboratedFinding> uncorroborated;
        QStringList staleFindings;  // pre-found accuracy findings (no model)
    };

    // INV-4 — sum-gate cap over the whole composed user prompt is the
    // inherited ReviewDialogBase::kPromptCapBytes (ANTS-2205).
    static constexpr qint64 kPerFileCapBytes = 64 * 1024;   // lane bodies
    static constexpr qint64 kPerDocCapBytes = 32 * 1024;    // cross-ref slices

    // ---- control / accessor surface (also used by the feature test) ----
    void setScope(ColdEyesEngine::Scope s);
    const QList<ColdEyesEngine::Lane> &engineLanes() const { return m_allLanes; }
    void setLaneSelected(const QString &laneName, bool on);
    bool isLaneSelected(const QString &laneName) const;
    const Results &results() const { return m_results; }
    QStringList lanesToReReview() const;
    void markFindingFixed(const IndieReviewEngine::CorroboratedFinding &f);
    void setFoldInMode(FoldInMode m) { m_foldInMode = m; }
    void setNarrativeText(const QString &md) { m_narrative = md; }

protected:
    QList<ReviewLane> derivePartition() override;
    LlmRequest        composeBrief(const ReviewLane &lane) override;
    void onAllReportsCollected(const QHash<QString, QString> &reportsById) override;
    void performFoldIn() override;

private:
    QList<ReviewLane> selectedReviewLanes() const;
    const ColdEyesEngine::Lane *laneByName(const QString &name) const;
    QStringList laneKeywords(const ColdEyesEngine::Lane &lane) const;
    QString     renderPriorFixes() const;
    QString     staleFindingLine(const QString &cite, const QString &lane) const;
    QString     assembleCappedPrompt(const ColdEyesEngine::Lane &lane,
                                     const QString &summary,
                                     const QStringList &staleFindings,
                                     const QString &fpBlock,
                                     const QString &laneBodies,
                                     const QString &xrefBodies) const;
    void buildControls();
    void onReReviewClicked();
    void rebuildPartitionPanel();
    void renderResultsWidget();

    ColdEyesEngine::Scope               m_scope = ColdEyesEngine::Scope::Default;
    QList<ColdEyesEngine::Lane>         m_allLanes;
    QSet<QString>                       m_deselected;
    QHash<QString, QStringList>         m_staleByLane;   // laneName → findings
    QList<ColdEyesEngine::PriorLoopFix> m_priorFixes;
    Results                             m_results;
    FoldInMode                          m_foldInMode = FoldInMode::PerFinding;
    QString                             m_narrative;

    // GUI (populated lazily; tests drive the logic via the public surface).
    QComboBox      *m_scopeCombo = nullptr;
    QListWidget    *m_laneList = nullptr;
    QPlainTextEdit *m_narrativeEdit = nullptr;
    QPlainTextEdit *m_resultsView = nullptr;
    QRadioButton   *m_perFindingRadio = nullptr;
};
