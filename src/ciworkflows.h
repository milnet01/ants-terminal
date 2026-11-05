#pragma once

// ci_report (ANTS-6399) — the workflow files' budgets and the keys its
// file-read checks need, read line by line from the working tree. No YAML
// library: a value it cannot read is unknown and listed in `unread`, never a
// guess (spec § 4.4, INV-3). Contract: docs/specs/ANTS-6399-ci-report-verb.md.

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace CiWorkflows {

struct Step {
    QString name;           // `name:`, empty when absent
    QString uses;
    QString run;            // single line, or a `|`/`>` block joined by \n
    QString ifExpr;
    QString cacheKey;       // `with: key:`
    QStringList envKeys;
    double budgetMin = -1;  // smaller of timeout-minutes and a leading shell guard; -1 unknown
    // GitHub's display name: `name:`, else "Run <uses>", else "Run <first run line>".
    QString displayName() const;
};

struct Job {
    QString id;
    QString name;           // `name:`, else the id
    QString runsOn;         // raw text, a list joined by ", "
    double budgetMin = -1;  // timeout-minutes; -1 absent or unreadable
    QStringList envKeys;
    QList<Step> steps;
};

struct Workflow {
    QString file;           // project-relative path
    QString name;           // top-level `name:`, else `file`
    QStringList envKeys;
    QList<Job> jobs;
};

struct Result {
    QList<Workflow> workflows;
    QJsonArray unread;      // [{file, job, field}]
};

// Every *.yml / *.yaml under <root>/.github/workflows, sorted by path.
Result read(const QString &root);
// One file's text; `rel` names it in `unread`.
void parseInto(Result &out, const QString &rel, const QString &text);

// The file GitHub's `workflowName` came from: its `name:`, else its path.
const Workflow *matchWorkflow(const Result &r, const QString &workflowName);
// A job by API name: exact `name` (else id), else "<name> (" for a matrix leg.
const Job *matchJob(const Workflow &w, const QString &apiJobName);
const Step *matchStep(const Job &j, const QString &apiStepName);

// A step's leading `timeout <N><s|m|h> ` in minutes, or -1 (spec § 4.4).
double shellGuardMinutes(const QString &run);

// JSON null for an empty string, the string otherwise: the reply's shape for
// a field that may be absent. Shared by the three ci_report files.
inline QJsonValue orNull(const QString &s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); }

}  // namespace CiWorkflows
