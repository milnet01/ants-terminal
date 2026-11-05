// ANTS-6399 TU 25/25 — the `ci_report` handler, and session_orient's `ci_report`
// block. Contract: docs/specs/ANTS-6399-ci-report-verb.md.
//
// The work is in three seams: CiWorkflows reads the workflow files, CiHistory
// runs `gh`, CiReport analyses. What lives here is the verb's own: its
// arguments, the answers decided before any `gh` call, one live run per
// project, and the saved report session_orient reads without a process.
//
// Every argument this file reads MUST be declared in the verb's
// inputSchema.properties in claudeintegration.cpp.

#include "remotecontrol.h"
#include "remotecontrol_internal.h"

#include "cihistory.h"
#include "cireport.h"
#include "ciworkflows.h"
#include "resolvedroot.h"
#include "secureio.h"
#include "sessionmemoryengine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

using namespace rcdetail;  // resolveRootCanonical

namespace {

constexpr int kDefaultDays = 30;
constexpr int kMaxDays = 90;
constexpr qint64 kLiveBudgetMs = qint64(120) * 1000;  // CiHistory::Options::deadlineMs

QJsonDocument refuse(const char *code, const QString &message, QJsonObject extra = {}) {
    extra[QStringLiteral("ok")] = false;
    extra[QStringLiteral("code")] = QString::fromLatin1(code);
    extra[QStringLiteral("error")] = QStringLiteral("ci_report: ") + message;
    return QJsonDocument(extra);
}

QString savedReportPath(const QString &root) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
         + QStringLiteral("/ants-terminal/ci-report/")
         + SessionMemoryEngine::cwdHash(root) + QStringLiteral(".json");
}

QJsonObject readSaved(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool hasWorkflowFiles(const QString &root) {
    return !QDir(root + QStringLiteral("/.github/workflows"))
                .entryList({QStringLiteral("*.yml"), QStringLiteral("*.yaml")}, QDir::Files)
                .isEmpty();
}

// The git remotes whose URL names github.com, read from the config files with
// no process: the repository's own, and a worktree's common one. `resolved`
// is true when `gh repo set-default` marked one (`gh-resolved`).
struct GithubRemotes {
    int count = 0;
    bool resolved = false;
};

GithubRemotes githubRemotes(const QString &root) {
    GithubRemotes found;
    QDir dir(root);
    QString gitPath;
    for (;;) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral(".git")))) {
            gitPath = dir.filePath(QStringLiteral(".git"));
            break;
        }
        if (!dir.cdUp()) return found;
    }
    QString gitDir = gitPath;
    if (QFileInfo(gitPath).isFile()) {   // a worktree or submodule: "gitdir: <path>"
        QFile f(gitPath);
        if (!f.open(QIODevice::ReadOnly)) return found;
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.startsWith(QLatin1String("gitdir:"))) return found;
        gitDir = QDir(QFileInfo(gitPath).path()).absoluteFilePath(line.mid(7).trimmed());
    }
    QStringList configs{gitDir + QStringLiteral("/config")};
    QFile common(gitDir + QStringLiteral("/commondir"));
    if (common.open(QIODevice::ReadOnly))
        configs << QDir(gitDir).absoluteFilePath(QString::fromUtf8(common.readAll()).trimmed())
                       + QStringLiteral("/config");
    for (const QString &path : configs) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        // Only a [remote "…"] section's url: a submodule's is another repository.
        // A remote counts once, however many url lines it has.
        bool inRemote = false, counted = false;
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith(QLatin1Char('['))) {
                inRemote = line.startsWith(QLatin1String("[remote "));
                counted = false;
            } else if (!inRemote) continue;
            else if (line.startsWith(QLatin1String("url")) && line.contains(QLatin1String("github.com"))) {
                if (!counted) ++found.count;
                counted = true;
            }
            else if (line.startsWith(QLatin1String("gh-resolved")))
                found.resolved = true;
        }
    }
    return found;
}

QJsonDocument noCi(const char *reason) {
    return QJsonDocument(QJsonObject{{QStringLiteral("ok"), true},
                                     {QStringLiteral("ci"), QStringLiteral("none")},
                                     {QStringLiteral("ci_none_reason"), QString::fromLatin1(reason)},
                                     {QStringLiteral("findings"), QJsonArray()}});
}

// One live run per project (spec § 4.5): an flock on a file beside the saved
// report, holding the run's start time. Each Claude session has its own
// ants-mcpd, so the lock is between processes; the kernel drops it with the
// process, so a crashed run never leaves it held.
class LiveLock {
public:
    ~LiveLock() { if (m_fd >= 0) ::close(m_fd); }

    bool acquire(const QString &path, qint64 nowMs, qint64 *heldSinceMs) {
        if (!ensurePrivateDir(QFileInfo(path).absolutePath())) return true;  // best effort
        m_fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_fd < 0) return true;
        if (::flock(m_fd, LOCK_EX | LOCK_NB) != 0) {
            char buf[32] = {};
            const ssize_t n = ::pread(m_fd, buf, sizeof buf - 1, 0);
            *heldSinceMs = n > 0 ? QByteArray(buf, int(n)).trimmed().toLongLong() : 0;
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        const QByteArray stamp = QByteArray::number(nowMs);
        // Best effort: a missing stamp only reads as running_since_ms 0.
        if (::ftruncate(m_fd, 0) == 0
            && ::pwrite(m_fd, stamp.constData(), size_t(stamp.size()), 0) < 0)
            qWarning("ci_report: could not stamp the live-run lock");
        return true;
    }

private:
    int m_fd = -1;
};

void writeSaved(const QString &path, const QJsonObject &reply) {
    if (!ensurePrivateDir(QFileInfo(path).absolutePath())) return;
    QSaveFile sf(path);
    const QByteArray body = QJsonDocument(reply).toJson(QJsonDocument::Compact);
    if (!sf.open(QIODevice::WriteOnly) || sf.write(body) != body.size() || !sf.commit()) {
        qWarning("ci_report: saved report write to %s failed: %s",
                 qUtf8Printable(path), qUtf8Printable(sf.errorString()));
        return;
    }
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
}

}  // namespace

QJsonDocument RemoteControl::cmdCiReport(const QJsonObject &req) {
    QJsonObject refusal;
    if (ants::requiredCallerCwd(m_roots, req, QStringLiteral("ci_report"), &refusal).isEmpty())
        return QJsonDocument(refusal);
    const QString root = resolveRootCanonical(m_roots, req);

    const QJsonValue sourceV = req.value(QStringLiteral("source"));
    const QString source = sourceV.isUndefined() ? QStringLiteral("live") : sourceV.toString();
    if (source != QLatin1String("live") && source != QLatin1String("saved"))
        return refuse("bad_args", QStringLiteral("source must be \"live\" or \"saved\""));
    const QJsonValue daysV = req.value(QStringLiteral("days"));
    const double daysD = daysV.isUndefined() ? kDefaultDays : daysV.toDouble(-1);
    if (daysD != std::floor(daysD) || daysD < 1 || daysD > kMaxDays)
        return refuse("bad_args", QStringLiteral("days must be a whole number from 1 to 90"));
    const int days = int(daysD);
    const QJsonValue workflowV = req.value(QStringLiteral("workflow"));
    if (!workflowV.isUndefined() && !workflowV.isString())
        return refuse("bad_args", QStringLiteral("workflow must be a string"));
    const QString workflow = workflowV.toString();

    const QString savedPath = savedReportPath(root);
    if (source == QLatin1String("saved")) {
        const QJsonObject saved = readSaved(savedPath);
        if (saved.isEmpty())
            return refuse("not_cached", QStringLiteral("no saved report; run source:\"live\""));
        return QJsonDocument(saved);
    }

    // Both "none" answers come before any process (INV-10).
    if (!hasWorkflowFiles(root)) return noCi("no_workflows");
    const GithubRemotes remotes = githubRemotes(root);
    if (remotes.count == 0) return noCi("no_github_remote");

    const QDateTime now = QDateTime::currentDateTimeUtc();
    LiveLock lock;
    qint64 since = 0;
    if (!lock.acquire(savedPath + QStringLiteral(".lock"), now.toMSecsSinceEpoch(), &since))
        return refuse("already_running", QStringLiteral("a live run for this project is in progress"),
            {{QStringLiteral("running_since_ms"), double(since)},
             {QStringLiteral("retry_after_ms"),
              double(std::max<qint64>(0, since + kLiveBudgetMs - now.toMSecsSinceEpoch()))}});

    const CiWorkflows::Result wf = CiWorkflows::read(root);
    CiHistory::Options opts;
    opts.days = days;
    opts.workflow = workflow;
    opts.now = now;
    const CiHistory::Result fetched = CiHistory::fetch(root, opts, [&](const QJsonObject &h) {
        return CiReport::analyse(h, wf, now, days).logJobs;
    });
    if (fetched.refusal == QLatin1String("runner_missing"))
        return refuse("runner_missing", QStringLiteral("gh did not start; install the GitHub CLI"));
    if (fetched.refusal == QLatin1String("github_unavailable"))
        return refuse("github_unavailable",
                      QStringLiteral("gh could not reach GitHub; see stderr_tail (gh auth status)"),
                      {{QStringLiteral("stderr_tail"), fetched.stderrTail}});

    const CiReport::Analysis a = CiReport::analyse(fetched.history, wf, now, days);
    QJsonArray unread = wf.unread;
    for (const QJsonValue &v : a.unread) unread.append(v);
    QJsonObject reply{
        {QStringLiteral("ok"), true},
        {QStringLiteral("ci"), QStringLiteral("github")},
        {QStringLiteral("repo"), fetched.repo},
        {QStringLiteral("generated_at"), now.toString(Qt::ISODate)},
        {QStringLiteral("window"), QJsonObject{{QStringLiteral("days"), days},
                                               {QStringLiteral("runs"), a.runs},
                                               {QStringLiteral("workflows"), a.workflows}}},
        {QStringLiteral("truncated"), !fetched.truncatedReason.isEmpty()},
        {QStringLiteral("truncated_reason"), fetched.truncatedReason.isEmpty()
                                                 ? QJsonValue() : QJsonValue(fetched.truncatedReason)},
        {QStringLiteral("findings"), a.findings},
        {QStringLiteral("counts"), a.counts},
        {QStringLiteral("unread"), unread},
        {QStringLiteral("fetch_errors"), fetched.fetchErrors}};
    // In a fork gh reads the `upstream` remote unless told otherwise, so say
    // which repository this is and how to read the other.
    if (remotes.count > 1 && !remotes.resolved)
        reply[QStringLiteral("repo_note")] = QStringLiteral(
            "This project has %1 GitHub remotes and gh has no default, so gh chose %2. "
            "To read another, run: gh repo set-default <owner/name>").arg(remotes.count).arg(fetched.repo);
    // Only a whole, default-window report replaces the saved one, so a narrow
    // run, or one missing jobs, never clears a real warning from
    // session_orient. A failed log, annotation or cache call loses a detail
    // or an advice finding, never a problem, so it does not block the save.
    if (workflow.isEmpty() && days == kDefaultDays && fetched.jobsComplete)
        writeSaved(savedPath, reply);
    return QJsonDocument(reply);
}

QJsonObject RemoteControl::buildCiReportBlock(const QString &rootCanonical, const QDateTime &nowUtc) {
    const QJsonObject saved = readSaved(savedReportPath(rootCanonical));
    const int problems = saved.value(QStringLiteral("counts")).toObject()
                             .value(QStringLiteral("problem")).toInt();
    const QJsonObject first = saved.value(QStringLiteral("findings")).toArray().at(0).toObject();
    if (problems < 1 || first.isEmpty()) return {};
    QString where = first.value(QStringLiteral("workflow")).toString();
    if (const QString job = first.value(QStringLiteral("job")).toString(); !job.isEmpty())
        where += QStringLiteral(" / ") + job;
    const int runs = int(first.value(QStringLiteral("runs")).toArray().size());
    const QString generated = saved.value(QStringLiteral("generated_at")).toString();
    const QDate made = QDateTime::fromString(generated, Qt::ISODate).toUTC().date();
    QJsonObject block{
        {QStringLiteral("problems"), problems},
        {QStringLiteral("first"), QStringLiteral("%1: %2, %3 run%4")
             .arg(first.value(QStringLiteral("check")).toString(), where)
             .arg(runs).arg(runs == 1 ? QString() : QStringLiteral("s"))},
        {QStringLiteral("generated_at"), generated},
        {QStringLiteral("hint"), QStringLiteral("ci_report {source:\"saved\"} for the list")}};
    if (made.isValid()) block[QStringLiteral("age_days")] = int(made.daysTo(nowUtc.toUTC().date()));
    return block;
}
