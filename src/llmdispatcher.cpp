// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llmdispatcher.h"

#include <algorithm>

LlmDispatcher::LlmDispatcher(int maxConcurrent, QObject *parent)
    : QObject(parent), m_max(std::clamp(maxConcurrent, 1, 4)) {}

void LlmDispatcher::setRunner(JobRunner runner) {
    m_runner = std::move(runner);
}

void LlmDispatcher::enqueue(const QList<LlmJob> &jobs) {
    if (jobs.isEmpty()) return;   // ANTS-5006 — no batch, so no allFinished
    m_cancelled = false;
    m_queue.append(jobs);
    pump();
}

void LlmDispatcher::cancelAll() {
    // Drop the queue and stop forwarding results. In-flight jobs belong to
    // the runner's owner, which aborts them if it must (ANTS-5009); each
    // completion that still arrives drains through pump() to the single
    // allFinished. Emits nothing itself, so a cancelAll() after the batch
    // finished cannot fire allFinished twice.
    m_cancelled = true;
    m_queue.clear();
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
