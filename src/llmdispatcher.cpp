// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llmdispatcher.h"

#include <algorithm>

LlmDispatcher::LlmDispatcher(int maxConcurrent, QObject *parent)
    : QObject(parent), m_max(std::clamp(maxConcurrent, 1, 4)) {
    installDefaultRunner();
}

void LlmDispatcher::setRunner(JobRunner runner) {
    m_runner = std::move(runner);
}

void LlmDispatcher::installDefaultRunner() {
    // Production runner: one LlmClient per job, parented to the dispatcher
    // so cancelAll can abort it. The completion callback hands the result
    // straight to the dispatcher (which forwards it and drops it — INV-8).
    m_runner = [this](const LlmJob &job,
                      std::function<void(const LlmResult &)> done) {
        auto *client = new LlmClient(this);
        m_activeClients.append(client);
        connect(client, &LlmClient::finished, this,
                [this, client, done](const LlmResult &r) {
                    m_activeClients.removeAll(client);
                    client->deleteLater();
                    done(r);
                });
        client->send(job.request);
    };
}

void LlmDispatcher::enqueue(const QList<LlmJob> &jobs) {
    m_cancelled = false;
    m_queue.append(jobs);
    pump();
}

void LlmDispatcher::cancelAll() {
    m_cancelled = true;
    m_queue.clear();
    // Abort any in-flight default-runner clients; their finished slot is
    // guarded by m_cancelled (no jobFinished emitted).
    const auto clients = m_activeClients;
    for (const QPointer<LlmClient> &c : clients)
        if (c) c->abort();
    // If nothing is in flight, the in-flight set is already drained.
    if (m_inFlight == 0)
        emit allFinished();
}

void LlmDispatcher::pump() {
    if (m_cancelled) {
        if (m_inFlight == 0)
            emit allFinished();
        return;
    }

    while (m_inFlight < m_max && !m_queue.isEmpty()) {
        const LlmJob job = m_queue.takeFirst();
        ++m_inFlight;
        const QString id = job.id;
        m_runner(job, [this, id](const LlmResult &r) {
            --m_inFlight;
            if (!m_cancelled)
                emit jobFinished(id, r);   // result forwarded, not stored
            pump();
        });
    }

    if (m_inFlight == 0 && m_queue.isEmpty())
        emit allFinished();
}
