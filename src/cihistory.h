#pragma once

// ci_report (ANTS-6399) — every network and process call the verb makes: `gh`,
// through HostExec so a Flatpak build runs the host's. Contract:
// docs/specs/ANTS-6399-ci-report-verb.md § 4.5. CiReport::analyse() reads what
// this gathers.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

namespace CiHistory {

struct Options {
    int days = 30;
    QString workflow;              // keep runs of this workflow name only
    QDateTime now = QDateTime::currentDateTimeUtc();
    int callCap = 300;
    int deadlineMs = 120 * 1000;
    int perCallMs = 20 * 1000;
    // The jobs calls stop with this much of either limit left, so the calls
    // after them (cache usage, annotations, logs) still run.
    int reserveCalls = 30;
    int reserveMs = 30 * 1000;
    int runLimit = 200;
    int maxLogs = 10;
    qint64 maxLogBytes = qint64(2) * 1024 * 1024;
};

struct Result {
    QString refusal;               // "", "runner_missing" or "github_unavailable"
    QString stderrTail;            // github_unavailable: the last 2 000 characters
    QString repo;                  // owner/name
    QJsonObject history;           // CiReport::analyse()'s input
    QString truncatedReason;       // "", "deadline", "call_cap" or "run_limit"
    QJsonArray fetchErrors;        // [{call, exit_code}]
};

// Given the history before logs, the jobs whose logs a finding would read.
using LogPicker = std::function<QList<qint64>(const QJsonObject &history)>;

Result fetch(const QString &root, const Options &opts, const LogPicker &pickLogs);

}  // namespace CiHistory
