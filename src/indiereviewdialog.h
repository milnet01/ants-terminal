// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// IndieReviewDialog — native in-app independent source-code review
// (ANTS-1258). A ReviewDialogBase (ANTS-1727) subclass that wraps the
// pure-function IndieReviewEngine: it partitions src/ into per-subsystem
// lanes (CLAUDE.md module map), composes a self-contained per-lane brief
// by reusing the engine's dispatch-shaped assembleBriefForDispatch (source
// bodies fenced, standards inlined, prior false-positives folded in),
// dispatches each lane to the configured LLM endpoint in parallel,
// corroborates the returned reports, optionally runs a cross-cutting
// synthesis pass, and folds findings into ROADMAP.md — all without
// spending Claude orchestration tokens. The source-code twin of
// ColdEyesDialog (ANTS-1721), which does the same for documentation.

#pragma once

#include "indiereviewengine.h"  // Lane, CorroboratedFinding
#include "reviewdialogbase.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

class QListWidget;
class QPlainTextEdit;
class QRadioButton;

class IndieReviewDialog : public ReviewDialogBase {
    Q_OBJECT
public:
    IndieReviewDialog(QString projectCwd, QWidget *parent, Config *config);

    enum class FoldInMode { PerFinding, Narrative };

    // Corroboration results, built in onAllReportsCollected.
    struct Results {
        QList<IndieReviewEngine::CorroboratedFinding> corroborated;
        QList<IndieReviewEngine::CorroboratedFinding> uncorroborated;
    };

    // INV-3 — sum-gate backstop over the composed user prompt is the
    // inherited ReviewDialogBase::kPromptCapBytes (ANTS-2205).

    // ---- control / accessor surface (also used by the feature test) ----
    const QList<IndieReviewEngine::Lane> &engineLanes() const { return m_allLanes; }
    void setLaneSelected(const QString &laneName, bool on);
    bool isLaneSelected(const QString &laneName) const;
    const Results &results() const { return m_results; }
    QStringList lanesToReReview() const;
    void setFoldInMode(FoldInMode m) { m_foldInMode = m; }
    void setNarrativeText(const QString &md) { m_narrative = md; }
    QString synthesisText() const { return m_synthText; }

protected:
    QList<ReviewLane> derivePartition() override;
    LlmRequest        composeBrief(const ReviewLane &lane) override;
    void onAllReportsCollected(const QHash<QString, QString> &reportsById) override;
    void performFoldIn() override;

    // Builds the synthesis job from the last reports and runs it through the
    // base dispatchOne (does NOT re-enter onAllReportsCollected — INV-5).
    // Public-via-test seam; in normal use it is reached only through the
    // endpoint-gated maybeDispatchSynthesis.
    void dispatchSynthesis();

private:
    QList<ReviewLane> selectedReviewLanes() const;
    const IndieReviewEngine::Lane *laneByName(const QString &name) const;
    QString assembleCappedPrompt(const QString &dispatchBrief) const;
    void buildControls();
    void onReReviewClicked();
    void maybeDispatchSynthesis();
    void rebuildPartitionPanel();
    void renderResultsWidget();

    QList<IndieReviewEngine::Lane> m_allLanes;
    QSet<QString>                  m_deselected;
    QHash<QString, QString>        m_lastReports;   // for synthesis
    Results                        m_results;
    FoldInMode                     m_foldInMode = FoldInMode::PerFinding;
    QString                        m_narrative;
    QString                        m_synthText;

    // GUI (populated lazily; tests drive the logic via the public surface).
    QListWidget    *m_laneList = nullptr;
    QPlainTextEdit *m_narrativeEdit = nullptr;
    QPlainTextEdit *m_resultsView = nullptr;
    QRadioButton   *m_perFindingRadio = nullptr;
};
