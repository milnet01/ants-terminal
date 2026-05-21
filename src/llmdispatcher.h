// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// LlmDispatcher — bounded-concurrency pool over LlmClient (ANTS-1727).
// Qt6::Core + Qt6::Network only; no Qt Widgets (INV-16). The review-dialog
// family enqueues per-lane / per-chunk jobs; the dispatcher runs at most
// `maxConcurrent` at once, emits jobFinished per job, and allFinished when
// the last completes. It retains no result text after emitting jobFinished
// (INV-8) — only in-flight LlmClient stream buffers consume memory.

#pragma once

#include "llmclient.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

struct LlmJob {
    QString    id;        // caller's lane / chunk key
    LlmRequest request;
};

class LlmDispatcher : public QObject {
    Q_OBJECT
public:
    // Test seam: a runner drives one job and reports completion via the
    // supplied callback. The default runner uses a real LlmClient.
    using JobRunner =
        std::function<void(const LlmJob &, std::function<void(const LlmResult &)>)>;

    explicit LlmDispatcher(int maxConcurrent = 2, QObject *parent = nullptr);

    void setRunner(JobRunner runner);   // tests inject a fake
    void enqueue(const QList<LlmJob> &jobs);
    void cancelAll();
    int  inFlight() const { return m_inFlight; }
    int  pending() const { return static_cast<int>(m_queue.size()); }
    int  maxConcurrent() const { return m_max; }

signals:
    void jobFinished(const QString &id, const LlmResult &result);
    void allFinished();

private:
    void pump();
    void installDefaultRunner();

    int            m_max = 2;
    int            m_inFlight = 0;
    bool           m_cancelled = false;
    QList<LlmJob>  m_queue;
    JobRunner      m_runner;
    QList<QPointer<LlmClient>> m_activeClients;  // default runner only
};
