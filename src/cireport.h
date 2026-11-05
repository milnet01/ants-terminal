#pragma once

// ci_report (ANTS-6399) — the analyser: run history in, findings out. Pure:
// no file, process or network. Contract: docs/specs/ANTS-6399-ci-report-verb.md
// § 4.3. Its own translation unit so a test feeds it fixtures directly.

#include "ciworkflows.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

namespace CiReport {

// `history` is what CiHistory::fetch() gathers, in gh's own JSON shapes:
//   runs        — `gh run list --json …` rows
//   jobs        — {"<runId>/<attempt>": the jobs API reply}
//   cache_usage — the cache usage API reply
//   annotations — {"<jobId>": the annotations API reply}
//   logs        — {"<jobId>": the job log's head and tail, its middle dropped}
struct Analysis {
    QJsonArray findings;       // sorted per spec § 4.2
    QJsonObject counts;        // {problem, advice}
    int runs = 0;              // runs in the window
    QJsonArray workflows;      // names seen in the window, sorted
    QList<qint64> logJobs;     // jobs whose log a finding reads, most useful first
    QJsonArray unread;         // [{file, job, field}]: a workflow or job no file matched
    QJsonArray files;          // [{file, workflow}]: the workflow files the checks read
};

// `atCommit` holds the workflow files as each run's commit held them; a
// run's budgets come from there, else from `workflows` (ANTS-6417).
Analysis analyse(const QJsonObject &history, const CiWorkflows::Result &workflows,
                 const QDateTime &now, int days, const CiWorkflows::BySha &atCommit = {});

}  // namespace CiReport
