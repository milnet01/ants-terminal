// ANTS-3794 § 2.5 — the backup health check. Spec:
// docs/specs/ANTS-3794-roadmap-store-backup.md
#include "roadmapbackuphealth.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QStringList>

namespace {

struct Job {
    const char *name;
    const char *timer;
};

// § 2.6 — the job each record belongs to, and the timer that runs it.
constexpr Job kJobs[] = {
    {"snapshot", "ants-roadmap-backup.timer"},
    {"export", "ants-roadmap-export.timer"},
};

QString recordPath(const QString &dir, const QString &job) {
    return dir + QStringLiteral("/roadmap-backup-") + job + QStringLiteral(".state");
}

// § 2.4 — key=value lines. Returns false when the file is missing or carries
// no `attempt` key, which § 2.5 reads as never_run.
bool readRecord(const QString &path, QHash<QString, QString> *out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw);
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq > 0)
            out->insert(line.left(eq), line.mid(eq + 1).trimmed());
    }
    return out->contains(QStringLiteral("attempt"));
}

}  // namespace

QString RoadmapBackupHealth::stateDir() {
    QString base = qEnvironmentVariable("XDG_STATE_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/ants-terminal");
}

QJsonObject RoadmapBackupHealth::assess(const QString &stateDir, const QDateTime &nowUtc,
                                        bool storeExists) {
    if (!storeExists)
        return {};

    QJsonObject jobs;
    QStringList hints;
    for (const Job &job : kJobs) {
        const QString name = QString::fromLatin1(job.name);
        const QString path = recordPath(stateDir, name);
        QHash<QString, QString> rec;

        // First matching row wins (§ 2.5's table order). An empty value reads
        // as absent (§ 2.4).
        QString state;
        if (!readRecord(path, &rec)) {
            state = QStringLiteral("never_run");
        } else if (!rec.value(QStringLiteral("error")).isEmpty()) {
            state = QStringLiteral("failing");
        } else {
            const QDateTime last =
                QDateTime::fromString(rec.value(QStringLiteral("success")), Qt::ISODate);
            if (!last.isValid() || last.secsTo(nowUtc) > kStaleAfterSecs)
                state = QStringLiteral("stale");
        }
        if (state.isEmpty())
            continue;

        jobs.insert(name, QJsonObject{
                              {QStringLiteral("state"), state},
                              {QStringLiteral("success"), rec.value(QStringLiteral("success"))},
                              {QStringLiteral("error"), rec.value(QStringLiteral("error"))},
                          });
        hints << QStringLiteral("%1: record %2, timer %3")
                     .arg(name, path, QString::fromLatin1(job.timer));
    }
    if (jobs.isEmpty())
        return {};
    return QJsonObject{
        {QStringLiteral("jobs"), jobs},
        {QStringLiteral("hint"),
         QStringLiteral("Roadmap store backup needs attention. %1. Check with "
                        "`systemctl --user status <timer>` and the job's journal.")
             .arg(hints.join(QStringLiteral("; ")))},
    };
}
