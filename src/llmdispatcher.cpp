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

    // ANTS-1755 — account for in-flight default-runner clients here.
    // LlmClient::abort() suppresses the client's finished() signal (it
    // nulls m_reply before aborting), so the completion callback that
    // would normally --m_inFlight and remove the client never fires.
    // Left as-is the dispatcher wedges: m_inFlight stuck > 0, allFinished
    // never emitted, and m_activeClients grows with dead clients across
    // cancel cycles. So drop + free each client and decrement in-flight
    // for it.
    const bool hadActiveClients = !m_activeClients.isEmpty();
    const auto clients = m_activeClients;
    m_activeClients.clear();
    for (const QPointer<LlmClient> &c : clients) {
        if (c) {
            c->abort();
            c->deleteLater();
        }
        if (m_inFlight > 0) --m_inFlight;
    }

    // Emit allFinished only when this call actually drained default-runner
    // work — a defensive cancelAll() after everything already finished
    // must not re-fire it (double-teardown). Custom-runner jobs (no client
    // handles) stay in flight and drive the single allFinished via their
    // own completion → pump().
    if (hadActiveClients && m_inFlight == 0)
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
