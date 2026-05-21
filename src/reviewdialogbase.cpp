// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "reviewdialogbase.h"

#include "config.h"
#include "dialogchrome.h"
#include "roadmapfoldin.h"

#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

ReviewDialogBase::ReviewDialogBase(QString projectCwd, QWidget *parent,
                                   Config *config)
    : QDialog(parent), m_projectCwd(std::move(projectCwd)), m_config(config) {
    setMinimumSize(640, 480);
    resize(900, 880);

    auto chrome = DialogChrome::install(this);
    QWidget *content = chrome.contentArea;

    // The content column stacks several tall/stretchy widgets (lane tabs,
    // the subclass results host, the fold-in panel). On a short window their
    // combined minimum heights overflow and the bottom widgets clip. Host the
    // whole column in a resizable scroll area: when the window is tall the
    // stretch fills it; when it's short a scrollbar appears instead of
    // clipping. Keeps every subclass (cold-eyes / test-audit / the deferred
    // audit-v2 + indie-review dialogs) safe without per-subclass tuning.
    auto *outer = new QVBoxLayout(content);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto *inner = new QWidget;
    scroll->setWidget(inner);
    m_contentLayout = new QVBoxLayout(inner);
    m_contentLayout->setContentsMargins(8, 8, 8, 8);
    m_contentLayout->setSpacing(6);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: gray; font-size: 11px;");
    m_statusLabel->setWordWrap(true);
    m_contentLayout->addWidget(m_statusLabel);

    m_laneTabs = new QTabWidget(this);
    m_contentLayout->addWidget(m_laneTabs, 1);

    m_dispatchBtn = new QPushButton(tr("Dispatch to AI"), this);
    connect(m_dispatchBtn, &QPushButton::clicked, this,
            &ReviewDialogBase::startDispatch);
    m_contentLayout->addWidget(m_dispatchBtn);

    m_resultsHost = new QWidget(this);
    (void)new QVBoxLayout(m_resultsHost);
    m_contentLayout->addWidget(m_resultsHost, 1);

    m_foldInBtn = new QPushButton(tr("Fold into ROADMAP"), this);
    connect(m_foldInBtn, &QPushButton::clicked, this,
            [this]() { performFoldIn(); });
    m_contentLayout->addWidget(m_foldInBtn);

    const int concurrency = m_config ? m_config->aiReviewConcurrency() : 2;
    m_dispatcher = new LlmDispatcher(concurrency, this);
    connect(m_dispatcher, &LlmDispatcher::jobFinished, this,
            &ReviewDialogBase::onJobFinished);
    connect(m_dispatcher, &LlmDispatcher::allFinished, this,
            &ReviewDialogBase::onAllFinished);

    // Default runner: one LlmClient per job, shared by batch dispatch and
    // dispatchOne so a test can swap both with one setJobRunner call.
    m_runner = [this](const LlmJob &job,
                      std::function<void(const LlmResult &)> done) {
        auto *client = new LlmClient(this);
        connect(client, &LlmClient::finished, this,
                [client, done](const LlmResult &r) {
                    done(r);
                    client->deleteLater();
                });
        client->send(job.request);
    };
    m_dispatcher->setRunner(m_runner);

    updateDispatchEnabled();
}

ReviewDialogBase::~ReviewDialogBase() {
    // Detach the dispatcher first: cancelAll may emit allFinished, and by
    // the time the base dtor runs the derived vtable is gone — routing it
    // to the pure-virtual onAllReportsCollected would abort.
    if (m_dispatcher) {
        disconnect(m_dispatcher, nullptr, this, nullptr);
        m_dispatcher->cancelAll();
    }
}

bool ReviewDialogBase::endpointDispatchable(const QString &endpoint) {
    return !endpoint.isEmpty() && LlmClient::isEndpointAllowed(endpoint);
}

void ReviewDialogBase::setJobRunner(LlmDispatcher::JobRunner runner) {
    m_runner = std::move(runner);
    if (m_dispatcher) m_dispatcher->setRunner(m_runner);
}

void ReviewDialogBase::updateDispatchEnabled() {
    const bool ok = m_config && endpointDispatchable(m_config->aiEndpoint());
    if (m_dispatchBtn) m_dispatchBtn->setEnabled(ok);
    if (!ok && m_statusLabel) {
        m_statusLabel->setText(
            tr("Dispatch disabled — set ai_endpoint (http/https) in "
               "config.json to enable AI review."));
    }
}

void ReviewDialogBase::setResultsWidget(QWidget *w) {
    if (!m_resultsHost) return;
    auto *lay = m_resultsHost->layout();
    if (!lay) return;
    while (QLayoutItem *it = lay->takeAt(0)) {
        if (QWidget *old = it->widget()) old->deleteLater();
        delete it;
    }
    if (w) {
        w->setParent(m_resultsHost);
        lay->addWidget(w);
    }
}

void ReviewDialogBase::runPartition() {
    setLanes(derivePartition());
}

void ReviewDialogBase::setLanes(const QList<ReviewLane> &lanes) {
    m_lanes = lanes;
    m_reports.clear();
    if (m_laneTabs) {
        m_laneTabs->clear();
        for (const ReviewLane &lane : m_lanes) {
            auto *view = new QTextEdit(m_laneTabs);
            view->setReadOnly(true);
            view->setPlainText(lane.summary);
            m_laneTabs->addTab(view, lane.title.isEmpty() ? lane.id : lane.title);
        }
    }
}

void ReviewDialogBase::startDispatch() {
    if (!m_config || !endpointDispatchable(m_config->aiEndpoint())) {
        updateDispatchEnabled();
        return;
    }
    m_reports.clear();
    QList<LlmJob> jobs;
    jobs.reserve(m_lanes.size());
    for (const ReviewLane &lane : m_lanes)
        jobs << LlmJob{ lane.id, composeBrief(lane) };
    m_dispatcher->enqueue(jobs);
}

void ReviewDialogBase::redispatch(const QStringList &laneIds) {
    QList<LlmJob> jobs;
    for (const ReviewLane &lane : m_lanes) {
        if (laneIds.contains(lane.id))
            jobs << LlmJob{ lane.id, composeBrief(lane) };
    }
    if (!jobs.isEmpty()) m_dispatcher->enqueue(jobs);
}

void ReviewDialogBase::dispatchOne(const LlmJob &job,
                                   std::function<void(const LlmResult &)> cb) {
    // Bypasses the dispatcher so it does NOT re-enter onAllReportsCollected
    // (e.g. test-audit's single synthesis follow-up).
    m_runner(job, std::move(cb));
}

void ReviewDialogBase::onJobFinished(const QString &id, const LlmResult &result) {
    m_reports.insert(id, result.text);
    if (m_laneTabs) {
        for (int i = 0; i < m_lanes.size(); ++i) {
            if (m_lanes[i].id == id) {
                if (auto *view = qobject_cast<QTextEdit *>(m_laneTabs->widget(i)))
                    view->setPlainText(result.text);
                break;
            }
        }
    }
}

void ReviewDialogBase::onAllFinished() {
    onAllReportsCollected(m_reports);
}

QString ReviewDialogBase::activeReleaseHeading() {
    return RoadmapFoldIn::findActiveReleaseHeading(m_projectCwd);
}

QList<int> ReviewDialogBase::allocateFoldInIds(int n) {
    m_lastFoldInError.clear();
    const QList<int> ids = RoadmapFoldIn::allocateIds(m_projectCwd, n);
    if (!ids.isEmpty()) return ids;

    // Surface a state-specific reason; write nothing.
    const auto insp = RoadmapFoldIn::inspectCounter(m_projectCwd);
    QString reason;
    switch (insp.state) {
        case RoadmapFoldIn::CounterState::Corrupt:
            reason = tr("ROADMAP counter is corrupt (\"%1…\") — fix "
                        ".roadmap-counter before folding in.")
                         .arg(insp.preview);
            break;
        case RoadmapFoldIn::CounterState::PermissionDenied:
            reason = tr("ROADMAP counter unreadable (permission denied).");
            break;
        case RoadmapFoldIn::CounterState::PathRefused:
            reason = tr("ROADMAP counter path refused (outside project root).");
            break;
        case RoadmapFoldIn::CounterState::EmptyOrAbsent:
            reason = tr("ROADMAP counter missing — could not allocate IDs.");
            break;
        case RoadmapFoldIn::CounterState::Ok:
            reason = tr("ID allocation failed (lock contention) — retry.");
            break;
    }
    m_lastFoldInError = reason;
    if (m_statusLabel) m_statusLabel->setText(reason);
    return {};
}

bool ReviewDialogBase::insertFoldInBlock(const QString &heading,
                                         const QString &block) {
    return RoadmapFoldIn::insertBlock(m_projectCwd, heading, block);
}
