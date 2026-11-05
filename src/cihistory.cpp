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

struct Call {
    bool started = false;
    int exitCode = -1;   // -1: killed at its time limit, or crashed
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

    // `gh args`; stdout past `maxBytes` (when positive) is cut and the call
    // stopped, which is a success.
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
        HostExec::start(p, QStringLiteral("gh"), args);
        if (!p.waitForStarted(kStartMs)) return c;
        c.started = true;
        const qint64 budget = std::min<qint64>(m_opts.perCallMs,
                                               m_opts.deadlineMs - m_clock.elapsed());
        QElapsedTimer t;
        t.start();
        bool killed = false, capped = false;
        while (p.state() != QProcess::NotRunning) {
            const qint64 left = budget - t.elapsed();
            if (left <= 0) { killed = true; break; }
            p.waitForFinished(int(std::min<qint64>(left, 100)));
            c.out += p.readAllStandardOutput();
            c.err += p.readAllStandardError();
            if (maxBytes > 0 && c.out.size() >= maxBytes) { capped = true; break; }
        }
        if (killed || capped) { p.kill(); p.waitForFinished(1000); }
        c.out += p.readAllStandardOutput();
        c.err += p.readAllStandardError();
        if (capped) { c.out.truncate(int(maxBytes)); c.exitCode = 0; return c; }
        if (!killed && p.exitStatus() == QProcess::NormalExit) c.exitCode = p.exitCode();
        return c;
    }

private:
    QString m_root;
    Options m_opts;
    QElapsedTimer m_clock;
    int m_calls = 0;
};

QString tail(const Call &c) {
    const QString s = QString::fromUtf8(c.err.isEmpty() ? c.out : c.err).trimmed();
    return s.right(kTailChars);
}

// The first two calls refuse the whole verb; later ones only lose a finding.
bool refuseOn(const Call &c, Result &res) {
    if (!c.started) { res.refusal = QStringLiteral("runner_missing"); return true; }
    if (c.exitCode != 0) {
        res.refusal = QStringLiteral("github_unavailable");
        res.stderrTail = tail(c);
        return true;
    }
    return false;
}

void noteError(Result &res, const QStringList &args, const Call &c) {
    res.fetchErrors.append(QJsonObject{{QStringLiteral("call"), QStringLiteral("gh ") + args.join(QLatin1Char(' '))},
                                       {QStringLiteral("exit_code"), c.started ? c.exitCode : -1}});
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
    res.history[QStringLiteral("runs")] = runs;

    const QString api = QStringLiteral("repos/") + res.repo;
    auto get = [&](const QString &path, qint64 maxBytes = -1) -> std::optional<QByteArray> {
        QStringList args{QStringLiteral("api"), path};
        // gh refuses to print a job log holding terminal escape codes without it.
        if (path.endsWith(QLatin1String("/logs"))) args << QStringLiteral("--allow-escape-sequences");
        const Call c = s.run(args, maxBytes);
        if (c.started && c.exitCode == 0) return c.out;
        noteError(res, args, c);
        return std::nullopt;
    };

    // Jobs, per run and per attempt, newest run first.
    QJsonObject jobs;
    bool stop = false;
    for (const QJsonValue &v : runs) {
        const QJsonObject r = v.toObject();
        if (r.value(QStringLiteral("status")).toString() != QLatin1String("completed")) continue;
        const qint64 id = qint64(r.value(QStringLiteral("databaseId")).toDouble());
        for (int a = 1; a <= std::max(1, r.value(QStringLiteral("attempt")).toInt(1)); ++a) {
            stop = s.spent(&res.truncatedReason, opts.reserveCalls, opts.reserveMs);
            if (stop) break;
            const QString key = QStringLiteral("%1/%2").arg(id).arg(a);
            if (const auto body = get(api + QStringLiteral("/actions/runs/%1/attempts/%2/jobs?per_page=100").arg(id).arg(a)))
                jobs[key] = QJsonDocument::fromJson(*body).object();
        }
        if (stop) break;
    }
    res.history[QStringLiteral("jobs")] = jobs;

    if (!s.spent(&res.truncatedReason))
        if (const auto body = get(api + QStringLiteral("/actions/cache/usage")))
            res.history[QStringLiteral("cache_usage")] = QJsonDocument::fromJson(*body).object();

    // Annotations for the jobs of each workflow's newest completed run.
    QJsonObject annotations;
    QSet<QString> seen;
    for (const QJsonValue &v : runs) {
        const QJsonObject r = v.toObject();
        const QString wf = r.value(QStringLiteral("workflowName")).toString();
        if (r.value(QStringLiteral("status")).toString() != QLatin1String("completed") || seen.contains(wf))
            continue;
        seen.insert(wf);
        const QString key = QStringLiteral("%1/%2")
            .arg(qint64(r.value(QStringLiteral("databaseId")).toDouble()))
            .arg(std::max(1, r.value(QStringLiteral("attempt")).toInt(1)));
        for (const QJsonValue &j : jobs.value(key).toObject().value(QStringLiteral("jobs")).toArray()) {
            if (s.spent(&res.truncatedReason)) break;
            const qint64 jobId = qint64(j.toObject().value(QStringLiteral("id")).toDouble());
            if (const auto body = get(api + QStringLiteral("/check-runs/%1/annotations").arg(jobId)))
                annotations[QString::number(jobId)] = QJsonDocument::fromJson(*body).array();
        }
    }
    res.history[QStringLiteral("annotations")] = annotations;

    QJsonObject logs;
    const QList<qint64> want = pickLogs ? pickLogs(res.history) : QList<qint64>();
    for (int i = 0; i < want.size() && i < opts.maxLogs; ++i) {
        if (s.spent(&res.truncatedReason)) break;
        if (const auto body = get(api + QStringLiteral("/actions/jobs/%1/logs").arg(want[i]), opts.maxLogBytes))
            logs[QString::number(want[i])] = QString::fromUtf8(*body);
    }
    res.history[QStringLiteral("logs")] = logs;

    if (res.truncatedReason.isEmpty() && runLimitHit) res.truncatedReason = QStringLiteral("run_limit");
    return res;
}

}  // namespace CiHistory
