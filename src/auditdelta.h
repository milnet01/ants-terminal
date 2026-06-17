// ANTS-1870 — pure since-last-run findings delta. Diffs this run's
// (changed-set) findings against the prior run's whole-tree findings to
// classify each as added / removed / carried-forward, and produces the
// merged whole-tree set the carry-forward SARIF + recorded sidecar persist.
//
// Operates entirely on the `QJsonArray findings` shape the runner builds
// (`{file, line, rule, severity, message, fp}`) — no struct⇄JSON bridge,
// no QProcess, no filesystem (INV-9 parity with AuditScope). Identity is
// the `fp` field (ANTS-1820 content fingerprint), line-insensitive.
//
// See docs/specs/ANTS-1870.md.

#pragma once

#include <QJsonArray>
#include <QSet>
#include <QString>

namespace AuditDelta {

struct DeltaResult {
    QJsonArray added;            // current findings whose fp ∉ priorOnChanged
    QJsonArray removed;          // priorOnChanged findings whose fp ∉ current
    QJsonArray carriedForward;   // (current ∩ priorOnChanged) ∪ priorOnUntouched
    QJsonArray merged;           // current ∪ priorOnUntouched — whole-tree
    int addedCount = 0;
    int removedCount = 0;
    int carriedForwardCount = 0;
};

// `current` — this run's findings (changed-set under since-last-run).
// `prior`   — the prior run's whole-tree findings (from the sidecar).
// `changedFiles` — the files re-scanned this run; prior findings on any
//                  other file are assumed still present (carried forward).
DeltaResult computeDelta(const QJsonArray &current,
                         const QJsonArray &prior,
                         const QSet<QString> &changedFiles);

}  // namespace AuditDelta
