// ANTS-1708 — fingerprint-keyed false-positive ledger. See auditfpledger.h.

#include "auditfpledger.h"

#include "secureio.h"
#include "configbackup.h"  // ConfigWriteLock — ANTS-1989

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ants {
namespace auditfp {

namespace {

QString ledgerPath(const QString &projectPath) {
    return projectPath + QStringLiteral("/.audit_cache/learned-fp.jsonl");
}

// Drop a leading `<path>:<line>[:<col>]:` location prefix. The descriptive
// remainder is what stays stable across line shifts. Anchored at start; only
// the first such prefix is removed.
QString stripLocation(const QString &message) {
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*\S+?:\d+(?::\d+)?:\s*)"));
    const QString trimmed = message.trimmed();
    const QRegularExpressionMatch m = re.match(trimmed);
    if (m.hasMatch()) return trimmed.mid(m.capturedLength()).trimmed();
    return trimmed;
}

}  // namespace

bool Entry::isValid() const {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{16}$"));
    return hex.match(fingerprint).hasMatch();
}

QString computeFingerprint(const QString &file,
                           const QString &checkId,
                           const QString &message) {
    // ANTS-4452 — multi-arg .arg() substitutes in ONE pass, so a literal
    // "%2"/"%3" inside `file` is never re-read as a placeholder. The
    // chained single-arg form is the one that would be, which is the
    // opposite of what this comment used to say. Mirrors computeDedup.
    const QString raw = QString(QStringLiteral("%1|%2|%3"))
                            .arg(file, checkId, stripLocation(message));
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}

QList<Entry> loadEntries(const QString &projectPath) {
    QList<Entry> out;
    const QString path = ledgerPath(projectPath);
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile()) return out;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray bytes = f.readAll();
    f.close();

    const QList<QByteArray> lines = bytes.split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) continue;
        const QJsonObject o = doc.object();

        Entry e;
        e.fingerprint = o.value(QStringLiteral("fingerprint")).toString();
        e.rule        = o.value(QStringLiteral("rule")).toString();
        e.reason      = o.value(QStringLiteral("reason")).toString();
        e.timestamp   = o.value(QStringLiteral("timestamp")).toString();
        if (e.isValid()) out.append(e);
    }
    return out;
}

QSet<QString> fingerprintSet(const QList<Entry> &entries) {
    QSet<QString> s;
    s.reserve(entries.size());
    for (const Entry &e : entries) s.insert(e.fingerprint);
    return s;
}

bool appendEntry(const QString &projectPath, Entry e) {
    if (projectPath.isEmpty()) return false;
    if (e.timestamp.isEmpty())
        e.timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!e.isValid()) return false;

    const QString dir = projectPath + QStringLiteral("/.audit_cache");
    if (!ensurePrivateDir(dir)) return false;   // ANTS-1988 — 0700 cache dir
    const QString path = ledgerPath(projectPath);

    // ANTS-1989 — hold the lock across the dedup-check AND the append, so a
    // concurrent CC session can't pass the same dedup gate before we append and
    // end up writing a duplicate row. The lock spans the read-modify-write that
    // the dedup makes implicit.
    ConfigWriteLock lock(path);

    // Dedup against what's already on disk — a learned FP only needs one row.
    if (fingerprintSet(loadEntries(projectPath)).contains(e.fingerprint))
        return true;

    QFile f(path);
    const bool fresh = !QFileInfo::exists(path) || QFileInfo(path).size() == 0;
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    if (fresh) {
        setOwnerOnlyPerms(f);
        f.write("# ants-audit learned false positives (JSONL, ANTS-1708). "
                "Keyed by line-independent content fingerprint.\n");
    }

    QJsonObject o;
    o[QStringLiteral("fingerprint")] = e.fingerprint;
    o[QStringLiteral("rule")]        = e.rule;
    o[QStringLiteral("reason")]      = e.reason;
    o[QStringLiteral("timestamp")]   = e.timestamp;
    // ANTS-1824 — one write() for record + newline. The file is opened
    // O_APPEND, so a single write lands atomically at EOF; the pre-fix
    // two-call form let a concurrent CC session's append interleave
    // between the JSON and its terminating '\n', corrupting the JSONL.
    const QByteArray line =
        QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    const bool wrote = f.write(line) == line.size();
    f.close();
    return wrote;
}

}  // namespace auditfp
}  // namespace ants
