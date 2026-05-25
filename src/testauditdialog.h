// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// TestAuditDialog — native in-app test-suite review (ANTS-1722). A
// ReviewDialogBase (ANTS-1727) subclass wrapping the pure-function
// TestAuditEngine: it partitions the test suite into chunks, composes a
// self-contained per-chunk brief (full dimension set + framework context
// + pre-pass findings + prior false-positives + inlined test bodies),
// dispatches each chunk to the configured LLM endpoint in parallel,
// writes the verbatim reports under .audit_cache/test_audit_<token>/,
// runs one synthesis follow-up, and folds findings into ROADMAP.md — all
// without spending Claude orchestration tokens. A partition_token via
// SessionMemoryEngine supports closing and resuming the dialog mid-audit
// (ANTS-1580).

#pragma once

#include "reviewdialogbase.h"
#include "testauditengine.h"

#include <QHash>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QRadioButton;

class TestAuditDialog : public ReviewDialogBase {
    Q_OBJECT
public:
    TestAuditDialog(QString projectCwd, QWidget *parent, Config *config);

    enum class FoldInMode { PerFinding, Narrative };

    // INV-4 — sum-gate cap over the whole composed user prompt.
    static constexpr qint64 kPromptCapBytes = 200 * 1024;
    static constexpr qint64 kPerFileCapBytes = 64 * 1024;

    // ---- control / accessor surface (also used by the feature test) ----
    QString partitionToken() const { return m_token; }
    const QVector<TestAuditEngine::Chunk> &chunks() const { return m_chunks; }
    QStringList dimensionsActive() const { return m_dimensionsActive; }
    void setDimensionsCsv(const QString &csv);
    TestAuditEngine::BriefResult briefFor(const QString &chunkId);
    const TestAuditEngine::SynthResult &lastSynth() const { return m_lastSynth; }
    const QHash<QString, QString> &collectedReports() const {
        return m_collectedReports;
    }
    void setFoldInMode(FoldInMode m) { m_foldInMode = m; }
    void setNarrativeText(const QString &md) { m_narrative = md; }
    void setActionable(const QJsonArray &a) { m_actionable = a; }

    // Resume (ANTS-1580).
    bool loadResume();
    QStringList unreviewedChunkIds() const;

    // Test seams.
    void setSessionMemBaseDir(const QString &dir) { m_sessionMemBaseDir = dir; }
    void setPartitionTokenForTest(const QString &t) { m_token = t; }

protected:
    QList<ReviewLane> derivePartition() override;
    LlmRequest        composeBrief(const ReviewLane &lane) override;
    void onAllReportsCollected(const QHash<QString, QString> &reportsById) override;
    void performFoldIn() override;
    // ANTS-1843 — refresh the partition (token + chunks + lanes) once before
    // the base dispatch loop, so briefFor() never re-partitions mid-loop.
    void prepareDispatch() override;

private:
    void    buildControls();
    void    runEnginePartition();
    void    rebuildPanel();
    void    renderResults();
    void    persistResumeState();
    QString reportsRelDir() const;
    QString assembleCappedPrompt(const TestAuditEngine::BriefResult &b) const;

    QString                          m_token;
    QVector<TestAuditEngine::Chunk>  m_chunks;
    QStringList                      m_dimensionsActive;
    QString                          m_framework;
    QString                          m_dimensionsCsv;   // "" = auto
    TestAuditEngine::SynthResult     m_lastSynth;
    QHash<QString, QString>          m_collectedReports;
    QString                          m_synthText;

    FoldInMode m_foldInMode = FoldInMode::PerFinding;
    QString    m_narrative;
    QJsonArray m_actionable;

    // Resume state loaded from session_memory.
    QString       m_persistedToken;
    QSet<QString> m_persistedCollected;
    QString       m_sessionMemBaseDir;   // empty = default cache dir

    // GUI.
    QLineEdit      *m_dimsEdit = nullptr;
    QPlainTextEdit *m_narrativeEdit = nullptr;
    QPlainTextEdit *m_resultsView = nullptr;
    QRadioButton   *m_perFindingRadio = nullptr;
};
