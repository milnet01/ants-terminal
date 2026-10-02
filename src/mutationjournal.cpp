// ANTS-5096 — see mutationjournal.h.
#include "mutationjournal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <cerrno>
#include <signal.h>     // kill()
#include <sys/types.h>  // pid_t

namespace {

QByteArray sha256Hex(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QString journalPath(const QString &dir, const QString &path) {
    return QDir(dir).filePath(QString::fromLatin1(sha256Hex(path.toUtf8())) +
                              QStringLiteral(".json"));
}

// kill(pid, 0) succeeds, or fails with EPERM, only for a live process.
bool pidAlive(qint64 pid) {
    if (pid <= 0) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

}  // namespace

QString MutationJournal::defaultDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/ants-terminal/mutation-journal");
}

bool MutationJournal::write(const QString &dir, const QString &path,
                            const QByteArray &original, const QByteArray &mutant) {
    if (!QDir().mkpath(dir)) return false;
    QJsonObject o;
    o[QStringLiteral("path")]          = path;
    o[QStringLiteral("original")]      = QString::fromLatin1(original.toBase64());
    o[QStringLiteral("mutant_sha256")] = QString::fromLatin1(sha256Hex(mutant));
    o[QStringLiteral("pid")]           = QCoreApplication::applicationPid();
    const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
    QSaveFile f(journalPath(dir, path));
    if (!f.open(QIODevice::WriteOnly)) return false;
    // It holds a copy of a source file, so only the owner reads it.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return f.write(body) == body.size() && f.commit();
}

void MutationJournal::clear(const QString &dir, const QString &path) {
    QFile::remove(journalPath(dir, path));
}

QList<MutationJournal::Recovered> MutationJournal::recover(const QString &dir) {
    QList<Recovered> out;
    const QStringList names =
        QDir(dir).entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString &name : names) {
        const QString jp = QDir(dir).filePath(name);
        QFile jf(jp);
        if (!jf.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(jf.readAll()).object();
        jf.close();
        if (pidAlive(o.value(QStringLiteral("pid")).toInteger())) continue;

        const QString path = o.value(QStringLiteral("path")).toString();
        const QByteArray original = QByteArray::fromBase64(
            o.value(QStringLiteral("original")).toString().toLatin1());
        const QByteArray mutantSha =
            o.value(QStringLiteral("mutant_sha256")).toString().toLatin1();
        if (path.isEmpty()) {   // unreadable journal: nothing it can restore
            QFile::remove(jp);
            continue;
        }

        Recovered r{path, QString()};
        QFile cur(path);
        if (!cur.open(QIODevice::ReadOnly)) {
            r.outcome = QStringLiteral("left_missing");
        } else {
            const QByteArray now = cur.readAll();
            cur.close();
            if (now == original) {
                QFile::remove(jp);   // already back: the restore ran
                continue;
            }
            if (sha256Hex(now) != mutantSha) {
                r.outcome = QStringLiteral("left_edited");
            } else {
                QSaveFile w(path);
                const bool ok = w.open(QIODevice::WriteOnly) &&
                                w.write(original) == original.size() && w.commit();
                r.outcome = ok ? QStringLiteral("restored")
                               : QStringLiteral("restore_failed");
            }
        }
        if (r.outcome != QLatin1String("restore_failed")) QFile::remove(jp);
        out.append(r);
    }
    return out;
}
