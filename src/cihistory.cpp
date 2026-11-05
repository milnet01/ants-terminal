// ci_report (ANTS-6399) — the fetch of spec § 4.5: the calls in order, each
// bounded, the whole fetch bounded, and what was fetched kept when a limit
// stops it (INV-8).

#include "cihistory.h"

#include "hostexec.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>

#include <algorithm>
#include <optional>

namespace CiHistory {

namespace {

constexpr int kStartMs = 5000;
constexpr int kTailChars = 2000;
constexpr int kErrBytes = 16 * 1024;  // stderr kept per call; its tail is all that is read
constexpr int kMaxJobPages = 10;   // 1 000 jobs a run attempt

struct Call {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;   // -1: not started, killed at its time limit, or crashed
    QByteArray out, err;
};

// The two limits shared by every call of one fetch.
class Session {
public:
    Session(const QString &root, const Options &opts) : m_root(root), m_opts(opts) {
        m_clock.start();
    }

    // True when a limit, less `reserveCalls` calls and `reserveMs`, is spent;
    // names that limit in `*reason` if it holds none yet.
    bool spent(QString *reason, int reserveCalls = 0, int reserveMs = 0) const {
        QString why;
        if (m_calls + reserveCalls >= m_opts.callCap) why = QStringLiteral("call_cap");
        else if (m_clock.elapsed() + reserveMs >= m_opts.deadlineMs) why = QStringLiteral("deadline");
        if (why.isEmpty()) return false;
        if (reason->isEmpty()) *reason = why;
        return true;
    }

    // `gh args`. With `maxBytes` positive, stdout keeps its first and last
    // maxBytes/2 bytes and drops the middle: a job log's cache line is near
    // its start and ctest's failure list at its end.
    Call run(const QStringList &args, qint64 maxBytes = -1) {
        ++m_calls;
        Call c;
        QProcess p;
        p.setWorkingDirectory(m_root);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GH_PROMPT_DISABLED"), QStringLiteral("1"));
        env.insert(QStringLiteral("GH_NO_UPDATE_NOTIFIER"), QStringLiteral("1"));
        env.insert(QStringLiteral("NO_COLOR"), QStringLiteral("1"));
        p.setProcessEnvironment(env);
        // The start wait counts against the call's own limit and the deadline.
        const qint64 budget = std::min<qint64>(m_opts.perCallMs,
                                               m_opts.deadlineMs - m_clock.elapsed());
        QElapsedTimer t;
        t.start();
        HostExec::start(p, QStringLiteral("gh"), args);
        if (!p.waitForStarted(int(std::clamp<qint64>(budget, 1, kStartMs)))) return c;
        c.started = true;
        bool killed = false, cut = false;
        QByteArray tail;
        const qint64 half = maxBytes / 2;
        auto take = [&] {
            QByteArray chunk = p.readAllStandardOutput();
            c.err += p.readAllStandardError();
            if (c.err.size() > kErrBytes) c.err = c.err.right(kErrBytes);
            if (maxBytes <= 0) { c.out += chunk; return; }
            if (c.out.size() < half) {
                const qint64 n = std::min<qint64>(half - c.out.size(), chunk.size());
                c.out += chunk.left(n);
                chunk.remove(0, n);
            }
            tail += chunk;
            if (tail.size() > half) { tail = tail.right(half); cut = true; }
        };
        while (p.state() != QProcess::NotRunning) {
            const qint64 left = budget - t.elapsed();
            if (left <= 0) { killed = true; break; }
            p.waitForFinished(int(std::min<qint64>(left, 100)));
            take();
        }
        if (killed) { p.kill(); p.waitForFinished(1000); c.timedOut = true; }
        take();
        c.out += cut ? QByteArray("\n[ci_report: middle of the log not kept]\n") + tail : tail;
        if (!killed && p.exitStatus() == QProcess::NormalExit) c.exitCode = p.exitCode();
        return c;
    }

private:
    QString m_root;
    Options m_opts;
    QElapsedTimer m_clock;
    int m_calls = 0;
};

QString errorTail(const Call &c) {
    const QString s = QString::fromUtf8(c.err.isEmpty() ? c.out : c.err).trimmed();
    return s.right(kTailChars);
}

// The first two calls refuse the whole verb; later ones only lose a finding.
bool refuseOn(const Call &c, Result &res) {
    if (!c.started) { res.refusal = QStringLiteral("runner_missing"); return true; }
    if (c.exitCode != 0) {
        res.refusal = QStringLiteral("github_unavailable");
        res.stderrTail = errorTail(c);
        return true;
    }
    return false;
}

// exit_code is -1 for a call that did not start, ran out of time or crashed;
// `reason` tells the first two apart.
void noteError(Result &res, const QStringList &args, const Call &c) {
    QJsonObject e{{QStringLiteral("call"), QStringLiteral("gh ") + args.join(QLatin1Char(' '))},
                  {QStringLiteral("exit_code"), c.exitCode}};
    if (!c.started) e[QStringLiteral("reason")] = QStringLiteral("not_started");
    else if (c.timedOut) e[QStringLiteral("reason")] = QStringLiteral("timeout");
    res.fetchErrors.append(e);
}

// The listed runs inside the window and the workflow filter, newest first.
QJsonArray keepRuns(const QJsonArray &listed, const Options &opts, bool *runLimitHit) {
    const QDateTime cutoff = opts.now.addSecs(-qint64(opts.days) * 86400);
    QJsonArray kept;
    QDateTime oldest;
    for (const QJsonValue &v : listed) {
        const QJsonObject o = v.toObject();
        const QDateTime created =
            QDateTime::fromString(o.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
        if (created.isValid() && (!oldest.isValid() || created < oldest)) oldest = created;
        if (!created.isValid() || created < cutoff) continue;
        if (!opts.workflow.isEmpty()
            && o.value(QStringLiteral("workflowName")).toString() != opts.workflow)
            continue;
        kept.append(o);
    }
    *runLimitHit = listed.size() >= opts.runLimit && oldest.isValid() && oldest >= cutoff;
    return kept;
}

// One fetch's calls after the first two, each phase its own method, sharing
// the session's limits and the result they write into.
class Fetcher {
public:
    Fetcher(Session &s, Result &res, const Options &opts)
        : m_s(s), m_res(res), m_opts(opts), m_api(QStringLiteral("repos/") + res.repo) {}

    // Jobs, per run and per attempt, newest run first. Stops with the
    // reserve left, so the calls after it still run (spec § 4.5).
    QJsonObject jobs(const QJsonArray &runs) {
        QJsonObject jobs;
        for (const QJsonValue &v : runs) {
            const QJsonObject r = v.toObject();
            if (!completed(r)) continue;
            for (int a = 1; a <= attemptOf(r); ++a) {
                QJsonObject reply;
                switch (attempt(runIdOf(r), a, &reply)) {
                case Attempt::Stopped: return jobs;
                case Attempt::Unknown: m_res.jobsComplete = false; break;
                case Attempt::Read: jobs[jobsKey(runIdOf(r), a)] = reply; break;
                }
            }
        }
        return jobs;
    }

    QJsonObject cacheUsage() {
        if (m_s.spent(&m_res.truncatedReason)) return {};
        const auto body = get(m_api + QStringLiteral("/actions/cache/usage"));
        return body ? QJsonDocument::fromJson(*body).object() : QJsonObject();
    }

    // Annotations for the jobs of each workflow's newest completed run.
    QJsonObject annotations(const QJsonArray &runs, const QJsonObject &jobs) {
        QJsonObject out;
        QSet<QString> seen;
        for (const QJsonValue &v : runs) {
            const QJsonObject r = v.toObject();
            const QString wf = r.value(QStringLiteral("workflowName")).toString();
            if (!completed(r) || seen.contains(wf)) continue;
            seen.insert(wf);
            const QJsonArray list = jobs.value(jobsKey(runIdOf(r), attemptOf(r))).toObject()
                                        .value(QStringLiteral("jobs")).toArray();
            for (const QJsonValue &j : list) {
                // Leave the logs their calls and half the time reserve (spec § 4.5).
                if (m_s.spent(&m_res.truncatedReason, m_opts.maxLogs, m_opts.reserveMs / 2))
                    return out;
                const qint64 jobId = qint64(j.toObject().value(QStringLiteral("id")).toDouble());
                if (const auto body =
                        get(m_api + QStringLiteral("/check-runs/%1/annotations?per_page=100").arg(jobId)))
                    out[QString::number(jobId)] = QJsonDocument::fromJson(*body).array();
            }
        }
        return out;
    }

    QJsonObject logs(const QList<qint64> &want) {
        QJsonObject out;
        for (int i = 0; i < want.size() && i < m_opts.maxLogs; ++i) {
            if (m_s.spent(&m_res.truncatedReason)) break;
            if (const auto body = get(m_api + QStringLiteral("/actions/jobs/%1/logs").arg(want[i]),
                                      m_opts.maxLogBytes))
                out[QString::number(want[i])] = QString::fromUtf8(*body);
        }
        return out;
    }

private:
    enum class Attempt { Read, Unknown, Stopped };

    static bool completed(const QJsonObject &run) {
        return run.value(QStringLiteral("status")).toString() == QLatin1String("completed");
    }

    // One `gh api` call; a failure is noted in fetch_errors.
    std::optional<QByteArray> get(const QString &path, qint64 maxBytes = -1) {
        QStringList args{QStringLiteral("api"), path};
        // gh refuses to print a job log holding terminal escape codes without it.
        if (path.endsWith(QLatin1String("/logs"))) args << QStringLiteral("--allow-escape-sequences");
        const Call c = m_s.run(args, maxBytes);
        if (c.started && c.exitCode == 0) return c.out;
        noteError(m_res, args, c);
        return std::nullopt;
    }

    // One run attempt's jobs, every page or none: a run read without its
    // later jobs could read a failed attempt as passed. Unknown when a call
    // failed or the pages ran out short; Stopped when a limit's reserve is
    // reached, which names it in truncated_reason.
    Attempt attempt(qint64 runId, int attemptNo, QJsonObject *reply) {
        if (spentJobs()) return Attempt::Stopped;
        const QString path = m_api + QStringLiteral("/actions/runs/%1/attempts/%2/jobs?per_page=100")
                                         .arg(runId).arg(attemptNo);
        const auto body = get(path);
        if (!body) return Attempt::Unknown;
        *reply = QJsonDocument::fromJson(*body).object();
        QJsonArray all = reply->value(QStringLiteral("jobs")).toArray();
        const int total = reply->value(QStringLiteral("total_count")).toInt(int(all.size()));
        for (int page = 2; all.size() < total && page <= kMaxJobPages; ++page) {
            if (spentJobs()) return Attempt::Stopped;
            const auto more = get(path + QStringLiteral("&page=%1").arg(page));
            if (!more) return Attempt::Unknown;   // get() noted the error
            const QJsonArray next =
                QJsonDocument::fromJson(*more).object().value(QStringLiteral("jobs")).toArray();
            if (next.isEmpty()) break;
            for (const QJsonValue &j : next) all.append(j);
        }
        if (all.size() < total) {
            // Pages ran out short of total_count with no failed call.
            m_res.fetchErrors.append(QJsonObject{{QStringLiteral("call"), QStringLiteral("gh api ") + path},
                                                 {QStringLiteral("exit_code"), 0},
                                                 {QStringLiteral("reason"), QStringLiteral("incomplete_pages")}});
            return Attempt::Unknown;
        }
        (*reply)[QStringLiteral("jobs")] = all;
        return Attempt::Read;
    }

    bool spentJobs() {
        return m_s.spent(&m_res.truncatedReason, m_opts.reserveCalls, m_opts.reserveMs);
    }

    Session &m_s;
    Result &m_res;
    const Options &m_opts;
    const QString m_api;
};

}  // namespace

Result fetch(const QString &root, const Options &opts, const LogPicker &pickLogs) {
    Result res;
    Session s(root, opts);

    const Call view = s.run({QStringLiteral("repo"), QStringLiteral("view"), QStringLiteral("--json"),
                             QStringLiteral("nameWithOwner,defaultBranchRef")});
    if (refuseOn(view, res)) return res;
    res.repo = QJsonDocument::fromJson(view.out).object()
                   .value(QStringLiteral("nameWithOwner")).toString();

    const Call list = s.run({QStringLiteral("run"), QStringLiteral("list"), QStringLiteral("--json"),
        QStringLiteral("databaseId,headSha,headBranch,conclusion,event,workflowName,createdAt,attempt,status"),
        QStringLiteral("--limit"), QString::number(opts.runLimit)});
    if (refuseOn(list, res)) return res;
    bool runLimitHit = false;
    const QJsonArray runs = keepRuns(QJsonDocument::fromJson(list.out).array(), opts, &runLimitHit);

    Fetcher f(s, res, opts);
    res.history[QStringLiteral("runs")] = runs;
    const QJsonObject jobs = f.jobs(runs);
    res.history[QStringLiteral("jobs")] = jobs;
    if (const QJsonObject usage = f.cacheUsage(); !usage.isEmpty())
        res.history[QStringLiteral("cache_usage")] = usage;
    res.history[QStringLiteral("annotations")] = f.annotations(runs, jobs);
    res.history[QStringLiteral("logs")] = f.logs(pickLogs ? pickLogs(res.history) : QList<qint64>());

    if (res.truncatedReason.isEmpty() && runLimitHit) res.truncatedReason = QStringLiteral("run_limit");
    return res;
}

}  // namespace CiHistory
