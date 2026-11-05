// ci_report (ANTS-6399) — the analyser of spec § 4.3. Each check is one
// function below, in the spec table's order. No message advises a push, a
// re-run, a retry or a lower budget (INV-5).

#include "cireport.h"

#include "cihistory.h"   // jobsKey, runIdOf, attemptOf

#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace CiReport {

namespace {

constexpr double kNearBudget = 0.80;         // near_budget: share of the budget
constexpr double kTrendRise = 1.25;          // budget_trend: newest median vs earlier
constexpr int kTrendNewest = 5;              // budget_trend: newest successful runs
constexpr qint64 kQueueWaitSecs = 5 * 60;    // queue_wait: median wait
constexpr double kGiB = 1024.0 * 1024 * 1024;
constexpr double kCacheLimitBytes = 10 * kGiB;  // GitHub's per-repository cache limit
constexpr double kCacheFull = 0.80;          // cache_size: share of the limit
constexpr int kMaxEvidence = 10;             // runs/shas per finding
constexpr int kMaxTestNames = 10;            // failing ctest names quoted by flaky

const QString kTimeoutCite = QStringLiteral("local-gate.md § 9 (timeout-minutes row)");
const QString kCachesCite = QStringLiteral("local-gate.md § 9 (caches row)");

struct StepRec { QString name, conclusion; qint64 secs = -1; };
struct RunRec;

struct JobRec {
    qint64 id = 0;
    QString name, conclusion;
    QDateTime created, started, completed;
    QList<StepRec> steps;
    const RunRec *run = nullptr;
    qint64 span() const {
        return started.isValid() && completed.isValid() ? started.secsTo(completed) : -1;
    }
};

struct RunRec {
    qint64 id = 0;
    QString sha, conclusion, status, event, workflow;
    QDateTime created;
    int attempt = 1;
    QList<QList<JobRec>> attempts;  // [attempt-1]; empty where not fetched
};

struct Evidence { QDateTime t; qint64 run; QString sha; };

struct Finding {
    QString check, severity, workflow, job, step;
    QString message;
    QList<Evidence> evidence;
    bool timing = false;   // carries `cache`
    qint64 logJob = 0;     // the newest job behind it, whose log it reads
    QDateTime logT;
    QStringList cites;
};

QDateTime when(const QJsonValue &v) {
    return QDateTime::fromString(v.toString(), Qt::ISODate);
}

bool failedConclusion(const QString &c) {
    return c == QLatin1String("failure") || c == QLatin1String("timed_out")
        || c == QLatin1String("startup_failure");
}

QString minutes(qint64 secs) {
    const qint64 m = secs / 60, s = secs % 60;
    return s ? QStringLiteral("%1m%2s").arg(m).arg(s, 2, 10, QLatin1Char('0'))
             : QStringLiteral("%1m").arg(m);
}

double median(QList<double> v) {
    if (v.isEmpty()) return 0;
    std::sort(v.begin(), v.end());
    const qsizetype n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

Evidence evidenceOf(const JobRec &j) {
    return {j.started.isValid() ? j.started : j.run->created, j.run->id, j.run->sha};
}
Evidence evidenceOf(const RunRec &r) { return {r.created, r.id, r.sha}; }

// One grouping key from several fields. U+001F, the unit separator, cannot
// occur in a workflow, job, step, event or SHA, so two keys never collide.
QString groupKey(std::initializer_list<QString> parts) {
    return QStringList(parts).join(QChar(0x1f));
}

// ---- reading the history ----------------------------------------------

QList<RunRec> readRuns(const QJsonObject &history, const QDateTime &now, int days) {
    const QDateTime cutoff = now.addSecs(-qint64(days) * 86400);
    const QJsonObject jobs = history.value(QStringLiteral("jobs")).toObject();
    QList<RunRec> runs;
    for (const QJsonValue &v : history.value(QStringLiteral("runs")).toArray()) {
        const QJsonObject o = v.toObject();
        RunRec r;
        r.id = CiHistory::runIdOf(o);
        r.sha = o.value(QStringLiteral("headSha")).toString();
        r.conclusion = o.value(QStringLiteral("conclusion")).toString();
        r.status = o.value(QStringLiteral("status")).toString();
        r.event = o.value(QStringLiteral("event")).toString();
        r.workflow = o.value(QStringLiteral("workflowName")).toString();
        r.created = when(o.value(QStringLiteral("createdAt")));
        r.attempt = CiHistory::attemptOf(o);
        if (!r.created.isValid() || r.created < cutoff) continue;
        for (int a = 1; a <= r.attempt; ++a) {
            QList<JobRec> list;
            const QJsonObject reply = jobs.value(CiHistory::jobsKey(r.id, a)).toObject();
            for (const QJsonValue &jv : reply.value(QStringLiteral("jobs")).toArray()) {
                const QJsonObject jo = jv.toObject();
                JobRec j;
                j.id = qint64(jo.value(QStringLiteral("id")).toDouble());
                j.name = jo.value(QStringLiteral("name")).toString();
                j.conclusion = jo.value(QStringLiteral("conclusion")).toString();
                j.created = when(jo.value(QStringLiteral("created_at")));
                j.started = when(jo.value(QStringLiteral("started_at")));
                j.completed = when(jo.value(QStringLiteral("completed_at")));
                for (const QJsonValue &sv : jo.value(QStringLiteral("steps")).toArray()) {
                    const QJsonObject so = sv.toObject();
                    const QDateTime s0 = when(so.value(QStringLiteral("started_at")));
                    const QDateTime s1 = when(so.value(QStringLiteral("completed_at")));
                    j.steps.append({so.value(QStringLiteral("name")).toString(),
                                    so.value(QStringLiteral("conclusion")).toString(),
                                    s0.isValid() && s1.isValid() ? s0.secsTo(s1) : -1});
                }
                list.append(j);
            }
            r.attempts.append(list);
        }
        runs.append(r);
    }
    std::sort(runs.begin(), runs.end(),
              [](const RunRec &a, const RunRec &b) { return a.created > b.created; });
    return runs;
}

// Back-pointers, set by the list's final owner once it stops moving.
void link(QList<RunRec> &runs) {
    for (RunRec &r : runs)
        for (QList<JobRec> &list : r.attempts)
            for (JobRec &j : list) j.run = &r;
}

void noteLog(Finding &f, const JobRec &j) {
    const QDateTime t = evidenceOf(j).t;
    if (!f.logJob || t > f.logT) { f.logJob = j.id; f.logT = t; }
}

// Each job instance with its workflow file's job, where one matches. A miss
// is named in `unread` once and leaves the job's budget unknown (INV-3).
struct Matched { const JobRec *job; const CiWorkflows::Job *wj; QString file; };

QList<Matched> matchJobs(const QList<RunRec> &runs, const CiWorkflows::Result &wf,
                         QJsonArray &unread) {
    QSet<QString> named;
    auto miss = [&](const QString &file, const QString &job, const QString &field,
                    const QString &workflow = QString()) {
        const QString key = groupKey({file, job, field, workflow});
        if (named.contains(key)) return;
        named.insert(key);
        QJsonObject u{{QStringLiteral("file"), CiWorkflows::orNull(file)},
                      {QStringLiteral("job"), CiWorkflows::orNull(job)},
                      {QStringLiteral("field"), field}};
        if (!workflow.isEmpty()) u[QStringLiteral("workflow")] = workflow;
        unread.append(u);
    };
    QList<Matched> out;
    for (const RunRec &r : runs) {
        const CiWorkflows::Workflow *w = CiWorkflows::matchWorkflow(wf, r.workflow);
        for (const QList<JobRec> &list : r.attempts)
            for (const JobRec &j : list) {
                // No file holds this workflow: `file` is null and `workflow` names it.
                if (!w) { miss(QString(), QString(), QStringLiteral("workflow"), r.workflow); out.append({&j, nullptr, {}}); continue; }
                const CiWorkflows::Job *wj = CiWorkflows::matchJob(*w, j.name);
                if (!wj) miss(w->file, j.name, QStringLiteral("job"));
                out.append({&j, wj, w->file});
            }
    }
    return out;
}

// ---- the checks ---------------------------------------------------------

QStringList failingCtestNames(const QString &log) {
    QStringList names;
    const int at = log.indexOf(QLatin1String("The following tests FAILED:"));
    if (at < 0) return names;
    static const QRegularExpression row(QStringLiteral(R"(^(?:\S+Z )?\s*\d+ - (\S+) \()"));
    const QStringList lines = log.mid(at).split(QLatin1Char('\n'));
    for (int i = 1; i < lines.size() && names.size() < kMaxTestNames; ++i) {
        const auto m = row.match(lines[i]);
        if (!m.hasMatch()) break;
        names.append(m.captured(1));
    }
    return names;
}

// flaky: one job, in one workflow, event and SHA, failed and later passed.
// The event keeps a pull request's merge-ref run apart from a push run of the
// same head SHA, which tests different code; the job keeps a skipped job's
// green run from reading as a pass. Grouped by the job and step that failed,
// so one flaky test across many commits is one finding.
void checkFlaky(const QList<RunRec> &runs, const QJsonObject &logs, QList<Finding> &out) {
    struct Inst { const JobRec *job; int attempt; };
    QMap<QString, QList<Inst>> byJob;
    for (const RunRec &r : runs)
        for (int a = 1; a <= r.attempts.size(); ++a)
            for (const JobRec &j : r.attempts.at(a - 1))
                byJob[groupKey({r.workflow, r.event, r.sha, j.name})]
                    .append({&j, a});

    QMap<QString, Finding> merged;
    QMap<QString, QSet<QString>> shasOf;
    for (QList<Inst> &list : byJob) {
        std::sort(list.begin(), list.end(), [](const Inst &a, const Inst &b) {
            return a.job->run->created != b.job->run->created
                       ? a.job->run->created < b.job->run->created
                       : a.attempt < b.attempt;
        });
        int lastPass = -1;
        for (int i = 0; i < list.size(); ++i)
            if (list[i].job->conclusion == QLatin1String("success")) lastPass = i;
        for (int i = 0; i < lastPass; ++i) {
            const JobRec &fj = *list[i].job;
            if (!failedConclusion(fj.conclusion)) continue;
            QString step;
            for (const StepRec &st : fj.steps)
                if (failedConclusion(st.conclusion)) { step = st.name; break; }
            const QString key = groupKey({fj.run->workflow, fj.name, step});
            Finding &f = merged[key];
            if (f.check.isEmpty()) {   // the first failure of this group
                f.check = QStringLiteral("flaky");
                f.severity = QStringLiteral("problem");
                f.workflow = fj.run->workflow;
                f.job = fj.name;
                f.step = step;
                f.cites = {QStringLiteral("testing.md § 7"), QStringLiteral("local-gate.md § 9 rule 3")};
            }
            f.evidence.append(evidenceOf(fj));
            f.evidence.append(evidenceOf(*list[lastPass].job));
            shasOf[key].insert(fj.run->sha);
            noteLog(f, fj);  // the newest failed job names the tests, if its log is read
        }
    }
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        Finding &f = it.value();
        const int commits = int(shasOf[it.key()].size());
        QString what = QStringLiteral("Job '%1'").arg(f.job);
        if (!f.step.isEmpty()) what += QStringLiteral(" (step '%1')").arg(f.step);
        f.message = QStringLiteral("%1 failed and then passed on the same commit, on %2 commit%3. "
                                   "The cause is in the test, the code it runs, or the runner "
                                   "it ran on.")
                        .arg(what).arg(commits).arg(commits == 1 ? QString() : QStringLiteral("s"));
        const QStringList tests =
            failingCtestNames(logs.value(QString::number(f.logJob)).toString());
        if (!tests.isEmpty())
            f.message += QStringLiteral(" Failing tests: %1.").arg(tests.join(QStringLiteral(", ")));
        out.append(f);
    }
}

// near_budget: a job or a step at 80% of its budget or more, measured from
// its own start, never the run's creation (INV-2).
void checkNearBudget(const QList<Matched> &jobs, QList<Finding> &out) {
    struct Acc { Finding f; int measured = 0; double worst = 0; qint64 worstSecs = 0; double budget = 0; };
    QMap<QString, Acc> acc;
    auto measure = [&](const Matched &m, const QString &step, qint64 secs, double budgetMin) {
        if (secs < 0 || budgetMin <= 0) return;
        Acc &a = acc[groupKey({m.job->run->workflow, m.job->name, step})];
        ++a.measured;
        const double ratio = double(secs) / (budgetMin * 60.0);
        if (ratio < kNearBudget) return;
        a.f.workflow = m.job->run->workflow;
        a.f.job = m.job->name;
        a.f.step = step;
        a.f.evidence.append(evidenceOf(*m.job));
        noteLog(a.f, *m.job);
        if (ratio > a.worst) { a.worst = ratio; a.worstSecs = secs; a.budget = budgetMin; }
    };
    for (const Matched &m : jobs) {
        if (!m.wj) continue;
        measure(m, QString(), m.job->span(), m.wj->budgetMin);
        for (const StepRec &s : m.job->steps)
            if (const CiWorkflows::Step *ws = CiWorkflows::matchStep(*m.wj, s.name))
                measure(m, s.name, s.secs, ws->budgetMin);
    }
    for (Acc &a : acc) {
        if (a.f.evidence.isEmpty()) continue;
        a.f.check = QStringLiteral("near_budget");
        a.f.severity = QStringLiteral("problem");
        a.f.timing = true;
        a.f.cites = {kTimeoutCite};
        a.f.message = QStringLiteral("%1 ran %2 against a %3 budget (%4%) in %5 of the last %6 runs.")
            .arg(a.f.step.isEmpty() ? QStringLiteral("Job") : QStringLiteral("Step"),
                 minutes(a.worstSecs), minutes(qint64(std::llround(a.budget * 60))))
            .arg(int(std::floor(a.worst * 100))).arg(a.f.evidence.size()).arg(a.measured);
        out.append(a.f);
    }
}

// budget_trend: a budgeted job's newest five successful runs against its
// earlier ones in the window, five at least.
void checkBudgetTrend(const QList<Matched> &jobs, QList<Finding> &out) {
    QMap<QString, QList<const Matched *>> byJob;
    for (const Matched &m : jobs)
        if (m.wj && m.wj->budgetMin > 0 && m.job->conclusion == QLatin1String("success")
            && m.job->span() >= 0)
            byJob[groupKey({m.job->run->workflow, m.job->name})].append(&m);
    for (QList<const Matched *> &list : byJob) {
        if (list.size() < 2 * kTrendNewest) continue;
        std::sort(list.begin(), list.end(), [](const Matched *a, const Matched *b) {
            return a->job->started < b->job->started;
        });
        QList<double> earlier, newest;
        for (int i = 0; i < list.size(); ++i)
            (i >= list.size() - kTrendNewest ? newest : earlier).append(double(list[i]->job->span()));
        const double before = median(earlier), after = median(newest);
        if (before <= 0 || after < before * kTrendRise) continue;
        Finding f;
        f.check = QStringLiteral("budget_trend");
        f.severity = QStringLiteral("advice");
        f.workflow = list.last()->job->run->workflow;
        f.job = list.last()->job->name;
        f.timing = true;
        f.logJob = list.last()->job->id;
        f.cites = {kTimeoutCite};
        for (int i = int(list.size()) - kTrendNewest; i < list.size(); ++i)
            f.evidence.append(evidenceOf(*list[i]->job));
        f.message = QStringLiteral("Median job time rose from %1 to %2 (+%3%) over its newest %4 "
                                   "successful runs, against a %5 budget.")
            .arg(minutes(qint64(before)), minutes(qint64(after)))
            .arg(int(std::lround((after / before - 1) * 100))).arg(kTrendNewest)
            .arg(minutes(qint64(std::llround(list.last()->wj->budgetMin * 60))));
        out.append(f);
    }
}

// scheduled_red: a workflow's newest scheduled run failed.
void checkScheduledRed(const QList<RunRec> &runs, QList<Finding> &out) {
    QMap<QString, QList<const RunRec *>> byWf;  // runs are newest first already
    for (const RunRec &r : runs)
        if (r.event == QLatin1String("schedule") && r.status == QLatin1String("completed"))
            byWf[r.workflow].append(&r);
    for (const QList<const RunRec *> &list : byWf) {
        if (!failedConclusion(list.first()->conclusion)) continue;
        Finding f;
        f.check = QStringLiteral("scheduled_red");
        f.severity = QStringLiteral("problem");
        f.workflow = list.first()->workflow;
        f.cites = {QStringLiteral("local-gate.md § 9 (slow-legs row)")};
        QString since = QStringLiteral("no green run in the window");
        for (const RunRec *r : list) {
            if (r->conclusion == QLatin1String("success")) {
                since = QStringLiteral("red since the green run of %1")
                            .arg(r->created.toUTC().date().toString(Qt::ISODate));
                break;
            }
            if (failedConclusion(r->conclusion)) f.evidence.append(evidenceOf(*r));
        }
        f.message = QStringLiteral("The newest scheduled run failed; %1.").arg(since);
        out.append(f);
    }
}

// queue_wait: a job's median wait from creation to start.
void checkQueueWait(const QList<Matched> &jobs, QList<Finding> &out) {
    struct Waited { const JobRec *job; qint64 secs; };
    QMap<QString, QList<Waited>> byJob;
    for (const Matched &m : jobs)
        if (m.job->created.isValid() && m.job->started.isValid())
            byJob[groupKey({m.job->run->workflow, m.job->name})]
                .append({m.job, m.job->created.secsTo(m.job->started)});
    for (const QList<Waited> &list : byJob) {
        QList<double> waits;
        for (const Waited &w : list) waits.append(double(w.secs));
        const double med = median(waits);
        if (med <= kQueueWaitSecs) continue;
        Finding f;
        f.check = QStringLiteral("queue_wait");
        f.severity = QStringLiteral("advice");
        f.workflow = list.first().job->run->workflow;
        f.job = list.first().job->name;
        for (const Waited &w : list)
            if (w.secs > kQueueWaitSecs) f.evidence.append(evidenceOf(*w.job));
        f.message = QStringLiteral("Jobs waited a median %1 between creation and start, over %2 runs.")
                        .arg(minutes(qint64(med))).arg(list.size());
        out.append(f);
    }
}

// annotation: a warning on a workflow's newest run that names a deprecation
// or a retiring runner image. Quotes GitHub's text; keeps no list of its own.
void checkAnnotations(const QList<RunRec> &runs, const QJsonObject &annotations,
                      QList<Finding> &out) {
    static const QRegularExpression words(
        QStringLiteral("deprecat|retire|will be removed|brownout"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> seenWf;
    for (const RunRec &r : runs) {
        if (r.status != QLatin1String("completed") || seenWf.contains(r.workflow)) continue;
        seenWf.insert(r.workflow);
        QSet<QString> quoted;
        for (const JobRec &j : r.attempts.at(r.attempt - 1))
            for (const QJsonValue &v : annotations.value(QString::number(j.id)).toArray()) {
                const QJsonObject a = v.toObject();
                if (a.value(QStringLiteral("annotation_level")).toString() != QLatin1String("warning"))
                    continue;
                const QString text = a.value(QStringLiteral("message")).toString().trimmed();
                if (!words.match(text + QLatin1Char(' ')
                                 + a.value(QStringLiteral("title")).toString()).hasMatch()
                    || quoted.contains(text))
                    continue;
                quoted.insert(text);
                Finding f;
                f.check = QStringLiteral("annotation");
                f.severity = QStringLiteral("advice");
                f.workflow = r.workflow;
                f.job = j.name;
                f.evidence.append(evidenceOf(r));
                f.message = QStringLiteral("GitHub warns: \"%1\"").arg(text);
                out.append(f);
            }
    }
}

void checkRunnerLatest(const CiWorkflows::Result &wf, QList<Finding> &out) {
    static const QRegularExpression latest(QStringLiteral(R"((?:^|[\s,\[])[\w.]+-latest\b)"));
    for (const CiWorkflows::Workflow &w : wf.workflows)
        for (const CiWorkflows::Job &j : w.jobs) {
            if (!latest.match(j.runsOn).hasMatch()) continue;
            Finding f;
            f.check = QStringLiteral("runner_latest");
            f.severity = QStringLiteral("advice");
            f.workflow = w.name;
            f.job = j.name;
            f.cites = {QStringLiteral("local-gate.md § 9 (warm-containers row, condition 1)")};
            f.message = QStringLiteral("runs-on '%1' moves to a new image when GitHub says so; "
                                       "a versioned label changes only with the workflow.")
                            .arg(j.runsOn);
            out.append(f);
        }
}

void checkCacheSize(const QJsonObject &usage, QList<Finding> &out) {
    const double bytes = usage.value(QStringLiteral("active_caches_size_in_bytes")).toDouble(-1);
    if (bytes < kCacheLimitBytes * kCacheFull) return;
    Finding f;
    f.check = QStringLiteral("cache_size");
    f.severity = QStringLiteral("advice");
    f.cites = {QStringLiteral("local-gate.md § 9 (caches row, prune)")};
    f.message = QStringLiteral("Actions caches hold %1 GB of GitHub's %2 GB (%3%); past the limit "
                               "GitHub evicts the least recently used.")
                    .arg(bytes / kGiB, 0, 'f', 1)
                    .arg(kCacheLimitBytes / kGiB, 0, 'f', 0)
                    .arg(int(std::floor(bytes / kCacheLimitBytes * 100)));
    out.append(f);
}

bool savesCache(const CiWorkflows::Step &s) {
    return s.uses.startsWith(QLatin1String("actions/cache@"))
        || s.uses.startsWith(QLatin1String("actions/cache/save@"));
}

void checkCacheKeys(const CiWorkflows::Result &wf, QList<Finding> &out) {
    QMap<QString, QStringList> jobsByPrefix;
    QMap<QString, QString> wfOf;
    for (const CiWorkflows::Workflow &w : wf.workflows)
        for (const CiWorkflows::Job &j : w.jobs)
            for (const CiWorkflows::Step &s : j.steps) {
                if (!savesCache(s) || s.cacheKey.isEmpty()) continue;
                const int last = int(s.cacheKey.lastIndexOf(QLatin1String("${{")));
                const QString prefix = last >= 0 ? s.cacheKey.left(last) : s.cacheKey;
                if (prefix.trimmed().isEmpty()) continue;
                const QString who = w.name + QStringLiteral(" / ") + j.name;
                if (!jobsByPrefix[prefix].contains(who)) jobsByPrefix[prefix].append(who);
                if (!wfOf.contains(prefix)) wfOf[prefix] = w.name;
            }
    for (auto it = jobsByPrefix.cbegin(); it != jobsByPrefix.cend(); ++it) {
        if (it.value().size() < 2) continue;
        Finding f;
        f.check = QStringLiteral("cache_key_shared");
        f.severity = QStringLiteral("advice");
        f.workflow = wfOf.value(it.key());
        f.job = it.value().join(QStringLiteral(", "));
        f.cites = {kCachesCite};
        f.message = QStringLiteral("%1 save cache keys starting '%2', so each restores the other's "
                                   "compiled output.").arg(it.value().join(QStringLiteral(" and ")), it.key());
        out.append(f);
    }
}

// An `if:` that still runs after an earlier step failed.
bool runsAfterFailure(QString ifExpr) {
    static const QRegularExpression afterFailure(
        QStringLiteral(R"(always\(\)|!cancelled\(\)|(?<!!)failure\(\))"));
    ifExpr.remove(QLatin1Char(' '));
    return afterFailure.match(ifExpr).hasMatch();
}

void checkCacheSave(const CiWorkflows::Result &wf, QList<Finding> &out) {
    for (const CiWorkflows::Workflow &w : wf.workflows)
        for (const CiWorkflows::Job &j : w.jobs)
            for (const CiWorkflows::Step &s : j.steps) {
                QString why;
                if (s.uses.startsWith(QLatin1String("actions/cache@")))
                    why = QStringLiteral("The combined actions/cache saves in a post step that does "
                                         "not run when the job fails, so the next run starts cold.");
                else if (s.uses.startsWith(QLatin1String("actions/cache/save@"))
                         && !runsAfterFailure(s.ifExpr))
                    why = QStringLiteral("This save step's if: does not run after a failure, so a "
                                         "failed step before it skips the save and the next run "
                                         "starts cold.");
                if (why.isEmpty()) continue;
                Finding f;
                f.check = QStringLiteral("cache_save_skipped");
                f.severity = QStringLiteral("advice");
                f.workflow = w.name;
                f.job = j.name;
                f.step = s.displayName();
                f.cites = {kCachesCite};
                f.message = why;
                out.append(f);
            }
}

// The words that run another command after them: a shell keyword, or a
// wrapper whose own options come first.
bool isWrapper(const QString &w) {
    static const QStringList words{
        QStringLiteral("env"), QStringLiteral("time"), QStringLiteral("sudo"),
        QStringLiteral("nice"), QStringLiteral("exec"), QStringLiteral("command"),
        QStringLiteral("stdbuf"), QStringLiteral("xvfb-run"), QStringLiteral("then"),
        QStringLiteral("do"), QStringLiteral("else"), QStringLiteral("if"),
        QStringLiteral("elif"), QStringLiteral("while"), QStringLiteral("until"),
        QStringLiteral("!")};
    return words.contains(w);
}

// A wrapper's options that take the next word as their value, so
// `sudo -u builder ctest` reaches ctest rather than stopping at `builder`.
// timeout's own value options are handled where timeout is.
bool takesValue(const QString &wrapper, const QString &opt) {
    static const QHash<QString, QStringList> options{
        {QStringLiteral("sudo"), {QStringLiteral("-u"), QStringLiteral("-g"), QStringLiteral("-C"),
                                  QStringLiteral("-D"), QStringLiteral("-h"), QStringLiteral("-p"),
                                  QStringLiteral("-r"), QStringLiteral("-t"), QStringLiteral("-T"),
                                  QStringLiteral("-U"), QStringLiteral("--user"),
                                  QStringLiteral("--group"), QStringLiteral("--chdir")}},
        {QStringLiteral("nice"), {QStringLiteral("-n"), QStringLiteral("--adjustment")}},
        {QStringLiteral("env"), {QStringLiteral("-u"), QStringLiteral("-C"), QStringLiteral("-S"),
                                 QStringLiteral("--unset"), QStringLiteral("--chdir"),
                                 QStringLiteral("--split-string")}},
        {QStringLiteral("time"), {QStringLiteral("-f"), QStringLiteral("-o"),
                                  QStringLiteral("--format"), QStringLiteral("--output")}},
        {QStringLiteral("stdbuf"), {QStringLiteral("-i"), QStringLiteral("-o"), QStringLiteral("-e")}},
        {QStringLiteral("xvfb-run"), {QStringLiteral("-e"), QStringLiteral("-f"), QStringLiteral("-n"),
                                      QStringLiteral("-p"), QStringLiteral("-s"), QStringLiteral("-w"),
                                      QStringLiteral("--error-file"), QStringLiteral("--auth-file"),
                                      QStringLiteral("--server-num"), QStringLiteral("--xauth-protocol"),
                                      QStringLiteral("--server-args"), QStringLiteral("--wait")}}};
    return options.value(wrapper).contains(opt);
}

// Shell words, with a quoted span kept inside its word and its quotes
// dropped, so `-s "-screen 0 640x480x8"` is two words, not four.
QStringList shellWords(const QString &command) {
    QStringList words;
    QString word;
    bool inWord = false;
    QChar quote;
    for (const QChar c : command) {
        if (!quote.isNull()) {
            if (c == quote) quote = QChar();
            else word += c;
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            inWord = true;
        } else if (c.isSpace()) {
            if (inWord) words.append(word);
            word.clear();
            inWord = false;
        } else {
            word += c;
            inWord = true;
        }
    }
    if (inWord) words.append(word);
    return words;
}

// A ctest command in `run` that sets --timeout and runs serially. A `\`
// continuation is joined first; then each command between &&, ||, ;, | and
// newlines is read on its own, past VAR=value words, a `(`, wrappers and
// their options (with the value an option takes), and
// `timeout [options] <duration>`. Words follow shell quoting. So cmake's --parallel,
// or a script named run-ctest.sh, says nothing about ctest.
bool serialCtestWithTimeout(QString run) {
    static const QRegularExpression sep(QStringLiteral(R"(\n|&&|\|\||;|\|)"));
    run.replace(QStringLiteral("\\\n"), QStringLiteral(" "));
    for (const QString &command : run.split(sep)) {
        QStringList words = shellWords(command);
        auto dropOptions = [&words](const QString &wrapper) {
            while (!words.isEmpty() && words.first().startsWith(QLatin1Char('-'))) {
                const QString opt = words.takeFirst();
                // timeout's -k/-s take a value of their own.
                const bool timeoutValue = wrapper == QLatin1String("timeout")
                    && (opt == QLatin1String("-k") || opt == QLatin1String("-s")
                        || opt == QLatin1String("--kill-after") || opt == QLatin1String("--signal"));
                if (!words.isEmpty() && (timeoutValue || takesValue(wrapper, opt)))
                    words.removeFirst();
            }
        };
        while (!words.isEmpty()) {
            QString &w = words.first();
            while (w.startsWith(QLatin1Char('('))) w.remove(0, 1);
            if (w.isEmpty() || (w.contains(QLatin1Char('=')) && !w.startsWith(QLatin1Char('-')))) {
                words.removeFirst();
            } else if (isWrapper(w)) {
                dropOptions(words.takeFirst());
            } else if (w == QLatin1String("timeout")) {
                dropOptions(words.takeFirst());
                if (!words.isEmpty()) words.removeFirst();  // the duration
            } else {
                break;
            }
        }
        if (words.isEmpty() || (words.first() != QLatin1String("ctest")
                                && !words.first().endsWith(QLatin1String("/ctest"))))
            continue;
        bool timeout = false, parallel = false;
        for (const QString &w : words) {
            timeout |= w == QLatin1String("--timeout") || w.startsWith(QLatin1String("--timeout="));
            parallel |= w.startsWith(QLatin1String("-j")) || w == QLatin1String("--parallel")
                     || w.startsWith(QLatin1String("--parallel="));
        }
        if (timeout && !parallel) return true;
    }
    return false;
}

void checkTestParallelism(const CiWorkflows::Result &wf, QList<Finding> &out) {
    const QString level = QStringLiteral("CTEST_PARALLEL_LEVEL");
    for (const CiWorkflows::Workflow &w : wf.workflows)
        for (const CiWorkflows::Job &j : w.jobs)
            for (const CiWorkflows::Step &s : j.steps) {
                // The level set in any env, inline, or exported earlier in the step.
                if (!serialCtestWithTimeout(s.run) || s.run.contains(level)
                    || w.envKeys.contains(level) || j.envKeys.contains(level)
                    || s.envKeys.contains(level))
                    continue;
                Finding f;
                f.check = QStringLiteral("test_parallelism");
                f.severity = QStringLiteral("advice");
                f.workflow = w.name;
                f.job = j.name;
                f.step = s.displayName();
                f.cites = {QStringLiteral("local-gate.md § 9 (test-parallelism row)")};
                f.message = QStringLiteral("ctest runs one test at a time here, and each test has "
                    "a --timeout. If the tests are isolated (no shared files, ports or global "
                    "state), ctest -j runs them side by side.");
                out.append(f);
            }
}

// ---- output -------------------------------------------------------------

QString cacheFromLog(const QString &log) {
    if (log.contains(QLatin1String("Cache restored from key"))) return QStringLiteral("hit");
    if (log.contains(QLatin1String("Cache not found for input keys"))) return QStringLiteral("miss");
    return QStringLiteral("unknown");
}

QJsonObject toJson(Finding f, const QJsonObject &logs) {
    std::stable_sort(f.evidence.begin(), f.evidence.end(),
                     [](const Evidence &a, const Evidence &b) { return a.t > b.t; });
    QJsonArray runs, shas;
    for (const Evidence &e : f.evidence) {
        if (runs.size() < kMaxEvidence && !runs.contains(QJsonValue(double(e.run))))
            runs.append(double(e.run));
        if (shas.size() < kMaxEvidence && !shas.contains(QJsonValue(e.sha))) shas.append(e.sha);
    }
    QJsonObject o{{QStringLiteral("check"), f.check},
                  {QStringLiteral("severity"), f.severity},
                  {QStringLiteral("workflow"), CiWorkflows::orNull(f.workflow)},
                  {QStringLiteral("job"), CiWorkflows::orNull(f.job)},
                  {QStringLiteral("step"), CiWorkflows::orNull(f.step)},
                  {QStringLiteral("message"), f.message},
                  {QStringLiteral("runs"), runs},
                  {QStringLiteral("shas"), shas},
                  {QStringLiteral("cites"), QJsonArray::fromStringList(f.cites)}};
    if (f.timing) {
        const QJsonValue log = logs.value(QString::number(f.logJob));
        o[QStringLiteral("cache")] = log.isString() ? cacheFromLog(log.toString())
                                                    : QStringLiteral("unknown");
    }
    return o;
}

}  // namespace

Analysis analyse(const QJsonObject &history, const CiWorkflows::Result &workflows,
                 const QDateTime &now, int days) {
    Analysis a;
    QList<RunRec> owned = readRuns(history, now, days);
    link(owned);
    // Jobs point into `owned`; read it only through this const view, so
    // nothing detaches it and moves the jobs out from under them.
    const QList<RunRec> &runs = owned;
    const QList<Matched> jobs = matchJobs(runs, workflows, a.unread);
    const QJsonObject logs = history.value(QStringLiteral("logs")).toObject();

    QList<Finding> found;
    checkFlaky(runs, logs, found);
    checkNearBudget(jobs, found);
    checkBudgetTrend(jobs, found);
    checkScheduledRed(runs, found);
    checkQueueWait(jobs, found);
    checkAnnotations(runs, history.value(QStringLiteral("annotations")).toObject(), found);
    checkRunnerLatest(workflows, found);
    checkCacheSize(history.value(QStringLiteral("cache_usage")).toObject(), found);
    checkCacheKeys(workflows, found);
    checkCacheSave(workflows, found);
    checkTestParallelism(workflows, found);

    // Logs the findings read, problems first: flaky, then near_budget, then trend.
    for (const char *check : {"flaky", "near_budget", "budget_trend"})
        for (const Finding &f : found)
            if (f.check == QLatin1String(check) && f.logJob && !a.logJobs.contains(f.logJob))
                a.logJobs.append(f.logJob);

    std::stable_sort(found.begin(), found.end(), [](const Finding &x, const Finding &y) {
        const auto key = [](const Finding &f) {
            return std::make_tuple(f.severity != QLatin1String("problem"), f.check, f.workflow,
                                   f.job, f.step);
        };
        return key(x) < key(y);
    });
    int problems = 0;
    for (const Finding &f : found) {
        a.findings.append(toJson(f, logs));
        problems += f.severity == QLatin1String("problem");
    }
    a.counts = {{QStringLiteral("problem"), problems},
                {QStringLiteral("advice"), int(found.size()) - problems}};
    a.runs = int(runs.size());
    QStringList names;
    for (const RunRec &r : runs)
        if (!names.contains(r.workflow)) names.append(r.workflow);
    names.sort();
    a.workflows = QJsonArray::fromStringList(names);
    return a;
}

}  // namespace CiReport
