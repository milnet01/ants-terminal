// ANTS-1870 — pure since-last-run findings delta. Diffs this run's
// (changed-set) findings against the prior run's whole-tree findings to
// classify each as added / removed / carried-forward, and produces the
// merged whole-tree set the carry-forward SARIF + recorded sidecar persist.
//
// Operates entirely on the `QJsonArray findings` shape the runner builds
// (`{file, line, rule, severity, message, fp, tool}`) — no struct⇄JSON bridge,
// no QProcess, no filesystem (INV-9 parity with AuditScope). Identity is
// the `fp` field (ANTS-1820 content fingerprint), line-insensitive.
//
// See docs/specs/ANTS-1870-audit-run-precise-since-last.md.

#pragma once

#include <QJsonArray>
#include <QSet>
#include <QString>

namespace AuditDelta {

struct DeltaResult {
    QJsonArray added;            // current ∖ prior
    QJsonArray removed;          // priorOnChanged ∖ current
    QJsonArray carriedForward;   // (current ∩ prior) ∪ (priorOnUntouched ∖ current)
    QJsonArray merged;           // current ∪ (priorOnUntouched ∖ current) — whole-tree
    int addedCount = 0;
    int removedCount = 0;
    int carriedForwardCount = 0;
};

// `current` — this run's findings (changed-set under since-last-run).
// `prior`   — the prior run's whole-tree findings (from the sidecar).
// `changedFiles` — the files re-scanned this run; prior findings on any
//                  other file are assumed still present (carried forward).
// `toolsRanOk`   — ANTS-6540: the tools that finished "ok" this run. A prior
//                  finding whose `tool` is not among them (skipped, failed,
//                  timed out, or untagged) was not re-checked, so it too is
//                  carried forward rather than counted as removed.
DeltaResult computeDelta(const QJsonArray &current,
                         const QJsonArray &prior,
                         const QSet<QString> &changedFiles,
                         const QSet<QString> &toolsRanOk);

// ANTS-5085 — bound the set the recorded sidecar persists to `maxEntries`,
// keeping the head. `merged` lists current findings before carried-forward
// ones, so the overflow shed is carried-forward first (ANTS-1870 § 2.5).
// Returns true when entries were dropped; the sidecar then records
// truncated:true.
bool capRecordedFindings(QJsonArray &merged, int maxEntries);

// ANTS-1706 — the findings in `current` whose `fp` is absent from
// `baseline`, in `current`'s order: what is new since a pinned baseline.
// Line-insensitive, like computeDelta, so a finding that only moved lines
// is not new.
QJsonArray newSinceBaseline(const QJsonArray &current,
                            const QJsonArray &baseline);

}  // namespace AuditDelta
