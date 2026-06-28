// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// ReviewDialogBase — shared QDialog scaffold for the v2 review-dialog
// family (ANTS-1727): ColdEyesDialog (ANTS-1721), TestAuditDialog
// (ANTS-1722), and the deferred audit-v2 / indie-review dialogs. Owns the
// partition panel, the per-lane brief tabs, the Dispatch button (→
// LlmDispatcher), a host slot for the subclass's results pane, and the
// Fold-into-ROADMAP button. Concrete dialogs fill four hooks
// (derivePartition / composeBrief / onAllReportsCollected / performFoldIn)
// and use the base services (setLanes / reports / redispatch / dispatchOne
// + the fold-in helpers).

#pragma once

#include "llmclient.h"
#include "llmdispatcher.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class Config;
class QLabel;
class QPushButton;
class QTabWidget;
class QVBoxLayout;

struct ReviewLane {
    QString id;
    QString title;
    QString summary;
};

class ReviewDialogBase : public QDialog {
    Q_OBJECT
public:
    // Arg order matches AuditDialog (mainwindow.cpp): cwd, parent, config.
    ReviewDialogBase(QString projectCwd, QWidget *parent, Config *config);
    ~ReviewDialogBase() override;

    // Non-GUI predicate, exposed for tests: dispatchable iff the endpoint
    // is non-empty AND http/https.
    static bool endpointDispatchable(const QString &endpoint);

protected:
    // ---- engine-specific hooks ----
    virtual QList<ReviewLane> derivePartition() = 0;                         // step 1
    virtual LlmRequest        composeBrief(const ReviewLane &lane) = 0;      // step 2 (bodies inlined)
    virtual void onAllReportsCollected(const QHash<QString, QString> &reportsById) = 0; // step 3
    virtual void performFoldIn() = 0;                                        // step 4 (subclass owns the sequence)
    // ANTS-1843 — called once at the top of startDispatch(), before any job
    // is composed. A subclass whose composeBrief() depends on a partition
    // token (TestAuditDialog) refreshes it here, so the per-lane loop never
    // re-partitions mid-dispatch and the enqueued set stays consistent.
    virtual void prepareDispatch() {}

    // ANTS-1843 — re-evaluate the Dispatch button when the window regains
    // activation (the user may have set ai_endpoint in config.json while
    // this modeless dialog was open; Config is reloaded in place upstream).
    void changeEvent(QEvent *e) override;

    // ---- base services ----
    const QString &projectCwd() const { return m_projectCwd; }
    Config *config() const { return m_config; }
    void    setResultsWidget(QWidget *w);
    void    runPartition();                                    // derivePartition() → setLanes()
    void    setLanes(const QList<ReviewLane> &lanes);          // (re)partition; clears reports()
    const QHash<QString, QString> &reports() const { return m_reports; }
    void    startDispatch();                                   // enqueue every lane
    void    redispatch(const QStringList &laneIds);            // re-run a subset; merges into reports()
    // Follow-up single job (e.g. test-audit synthesis) that does NOT
    // re-enter onAllReportsCollected:
    void    dispatchOne(const LlmJob &job, std::function<void(const LlmResult &)> cb);

    // Fold-in helpers the subclass composes inside its performFoldIn():
    QString    activeReleaseHeading();                         // RoadmapFoldIn::findActiveReleaseHeading
    QList<int> allocateFoldInIds(int n);                       // RoadmapFoldIn::allocateIds; [] + reason on fail
    bool       insertFoldInBlock(const QString &heading, const QString &block);
    const QString &lastFoldInError() const { return m_lastFoldInError; }

    // Test seam: replace the runner used by both batch dispatch and
    // dispatchOne. Default = a real LlmClient per job.
    void setJobRunner(LlmDispatcher::JobRunner runner);

    QLabel *statusLabel() const { return m_statusLabel; }

private:
    void updateDispatchEnabled();
    void onJobFinished(const QString &id, const LlmResult &result);
    void onAllFinished();

    QString  m_projectCwd;
    Config  *m_config = nullptr;

    LlmDispatcher           *m_dispatcher = nullptr;
    LlmDispatcher::JobRunner m_runner;        // shared by batch + dispatchOne
    // ANTS-2111 — clients spawned by m_runner are parented to this dialog but
    // bypass the dispatcher's m_activeClients, so cancelAll() can't abort them.
    // Track them here and abort in ~ReviewDialogBase before cancelAll so a
    // synchronous finished() from QNetworkReply::abort() can't re-enter a
    // half-destroyed dialog (the close-mid-review UAF).
    QList<QPointer<LlmClient>> m_activeClients;
    QList<ReviewLane>        m_lanes;
    QHash<QString, QString>  m_reports;       // accumulated, keyed by lane id
    QString                  m_lastFoldInError;

    QLabel      *m_statusLabel = nullptr;
    QTabWidget  *m_laneTabs = nullptr;
    QPushButton *m_dispatchBtn = nullptr;
    QPushButton *m_foldInBtn = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
    QWidget     *m_resultsHost = nullptr;
};
