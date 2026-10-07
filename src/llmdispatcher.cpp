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
    m_batchOpen = true;
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
    ++m_generation;   // ANTS-5105 — enqueue() clears m_cancelled; this does not
    m_queue.clear();
}

void LlmDispatcher::pump() {
    // ANTS-5105 — a runner that calls done before returning re-enters
    // pump(). The nested call returns at once and the loop below carries
    // on, so runner calls never nest however long the queue is.
    if (m_pumping) return;
    m_pumping = true;
    while (!m_cancelled && m_inFlight < m_max && !m_queue.isEmpty()) {
        const LlmJob job = m_queue.takeFirst();
        ++m_inFlight;
        const QString id = job.id;
        const quint64 gen = m_generation;
        m_runner(job, [this, id, gen](const LlmResult &r) {
            --m_inFlight;
            // ANTS-5105 — a later enqueue() clears m_cancelled, so the batch
            // generation is what keeps a cancelled job's late result out of
            // the next round.
            if (!m_cancelled && gen == m_generation)
                emit jobFinished(id, r);   // result forwarded, not stored
            pump();
        });
    }

    m_pumping = false;

    // Only the outermost frame gets here, so allFinished is sent once
    // (ANTS-5000) and is the last thing pump() does: a slot may delete the
    // dispatcher. cancelAll() emptied the queue, so a cancelled batch ends
    // here too once in-flight drains.
    if (m_batchOpen && m_inFlight == 0 && m_queue.isEmpty()) {
        m_batchOpen = false;
        emit allFinished();
    }
}
