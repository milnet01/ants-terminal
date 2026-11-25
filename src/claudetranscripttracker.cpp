#include "claudetranscripttracker.h"
#include "claudetranscriptwatch.h"

#include <QDateTime>
#include <QFileInfo>

ClaudeTranscriptTracker::ClaudeTranscriptTracker(QObject *parent) : QObject(parent) {
    m_rescanDebounce.setSingleShot(true);
    m_rescanDebounce.setInterval(kRescanDebounceMs);
    connect(&m_rescanDebounce, &QTimer::timeout, this, &ClaudeTranscriptTracker::rescan);
    // ANTS-1285 — one shared watch per transcript; filter on our own path.
    connect(&ClaudeTranscriptWatch::instance(), &ClaudeTranscriptWatch::changed,
            this, [this](const QString &path) {
                if (path == m_transcriptPath) m_rescanDebounce.start();
            });
}

ClaudeTranscriptTracker::~ClaudeTranscriptTracker() {
    if (auto *watch = ClaudeTranscriptWatch::existing())
        watch->release(m_transcriptPath);
}

void ClaudeTranscriptTracker::setTranscriptPath(const QString &path) {
    if (path == m_transcriptPath) return;
    ClaudeTranscriptWatch::instance().release(m_transcriptPath);
    m_transcriptPath = path;
    // A re-bind always reparses: after a tab-switch round trip the file's
    // mtime and size may not have ticked, and poll() would skip the rescan.
    m_lastRescanMtimeMs   = 0;
    m_lastRescanSizeBytes = 0;
    rebindWalkState();
    ClaudeTranscriptWatch::instance().acquire(m_transcriptPath);
    m_rescanDebounce.stop();  // ANTS-5050 — this rescan supersedes a pending one
    rescan();
}

void ClaudeTranscriptTracker::rescan() {
    // ANTS-1458 phase 2 — sample the change signal BEFORE the read, not
    // after. The read goes to the file's current EOF; if Claude appends
    // during it (a TaskCreate burst writes every ~100 ms), a signal sampled
    // afterwards would match the new bytes the read missed, and poll() would
    // strand them until an unrelated write. A pre-read sample leaves any
    // such append strictly newer, so the next poll() re-parses.
    qint64 mtimeMs = 0, sizeBytes = 0;
    bool haveSignal = false;
    if (!m_transcriptPath.isEmpty()) {
        const QFileInfo fi(m_transcriptPath);
        if (fi.exists()) {
            mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
            sizeBytes = fi.size();
            haveSignal = true;
        }
    }

    const bool changed = refold();

    // The watcher drops the path on an atomic rewrite; re-add it if it
    // disappeared between rescans.
    ClaudeTranscriptWatch::instance().ensure(m_transcriptPath);

    if (changed) emit tasksChanged();

    if (haveSignal) {
        m_lastRescanMtimeMs   = mtimeMs;
        m_lastRescanSizeBytes = sizeBytes;
    }
}

void ClaudeTranscriptTracker::poll() {
    if (m_transcriptPath.isEmpty()) return;
    const QFileInfo fi(m_transcriptPath);
    if (!fi.exists()) return;
    // ANTS-2002 — the watch may have been lost (file absent at bind time,
    // atomic rename on save, inotify exhaustion). Re-add it and reparse.
    if (ClaudeTranscriptWatch::instance().ensure(m_transcriptPath)) {
        rescan();
        return;
    }
    // ANTS-1458 phase 2 — re-parse when EITHER signal moved. mtime alone
    // misses a same-millisecond append; size catches it. otherSourcesMoved()
    // has a side effect (the bg tracker folds helper events into its
    // accumulator) and runs only when mtime and size are unchanged; keep it
    // last in the condition.
    const qint64 mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
    const qint64 sizeBytes = fi.size();
    if (mtimeMs == m_lastRescanMtimeMs && sizeBytes == m_lastRescanSizeBytes
        && !otherSourcesMoved())
        return;
    rescan();
}
