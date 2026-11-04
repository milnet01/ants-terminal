// ANTS-4513 § 4.1 — the feedback corpus tables' typed surface. Its own file
// because nothing else in roadmapstore.cpp reads or writes these tables; the
// DDL stays there, shared by createSchema() and the 4 → 5 rung.
#include "roadmapstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace {

bool needTx(bool inTx, QString *error) {
    if (inTx) return true;
    if (error) *error = QStringLiteral("feedback writes need an open transaction (begin())");
    return false;
}

bool fail(const QSqlQuery &q, QString *error) {
    if (error) *error = q.lastError().text();
    return false;
}

QVariant nullIfEmpty(const QString &s) {
    return s.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(s);
}

bool insertRows(QSqlDatabase &db, qint64 reporterId,
                const QVector<RoadmapStore::FeedbackFindingRow> &rows, QString *error) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO feedback_finding (reporter_id, ordinal, heading, heading_line,"
        " entry_heading, filed_on, triage, ids) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    for (const auto &r : rows) {
        q.addBindValue(reporterId);
        q.addBindValue(r.ordinal);
        q.addBindValue(r.heading);
        q.addBindValue(r.headingLine);
        q.addBindValue(r.entryHeading);
        q.addBindValue(nullIfEmpty(r.filedOn));
        q.addBindValue(r.triage);
        q.addBindValue(RoadmapStore::canonicalJson(QJsonArray::fromStringList(r.ids)));
        if (!q.exec()) return fail(q, error);
    }
    return true;
}

}  // namespace

std::optional<RoadmapStore::FeedbackReporter>
RoadmapStore::feedbackReporter(const QString &exportPath, QString *error) const {
    if (error) error->clear();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT reporter_id, content, revision, export_sha256 FROM feedback_reporter"
        " WHERE export_path = ?"));
    q.addBindValue(exportPath);
    if (!q.exec()) {
        fail(q, error);
        return std::nullopt;
    }
    if (!q.next()) return std::nullopt;
    FeedbackReporter r;
    r.reporterId   = q.value(0).toLongLong();
    r.exportPath   = exportPath;
    r.content      = q.value(1).toString();
    r.revision     = q.value(2).toInt();
    r.exportSha256 = q.value(3).toString();
    return r;
}

std::optional<qint64> RoadmapStore::insertFeedbackReporter(
    const QString &exportPath, const QString &content, const QString &exportSha256,
    const QString &now, const QVector<FeedbackFindingRow> &rows, QString *error) {
    if (!needTx(m_inTransaction, error)) return std::nullopt;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO feedback_reporter (export_path, content, export_sha256,"
        " created_at, updated_at) VALUES (?, ?, ?, ?, ?)"));
    q.addBindValue(exportPath);
    q.addBindValue(content);
    q.addBindValue(nullIfEmpty(exportSha256));
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) {
        fail(q, error);
        return std::nullopt;
    }
    const qint64 id = q.lastInsertId().toLongLong();
    if (!insertRows(m_db, id, rows, error)) return std::nullopt;
    return id;
}

bool RoadmapStore::updateFeedbackContent(qint64 reporterId, const QString &content,
                                         const QString &now,
                                         const QVector<FeedbackFindingRow> &rows,
                                         QString *error) {
    if (!needTx(m_inTransaction, error)) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE feedback_reporter SET content = ?, revision = revision + 1,"
        " updated_at = ? WHERE reporter_id = ?"));
    q.addBindValue(content);
    q.addBindValue(now);
    q.addBindValue(reporterId);
    if (!q.exec()) return fail(q, error);
    QSqlQuery d(m_db);
    d.prepare(QStringLiteral("DELETE FROM feedback_finding WHERE reporter_id = ?"));
    d.addBindValue(reporterId);
    if (!d.exec()) return fail(d, error);
    return insertRows(m_db, reporterId, rows, error);
}

bool RoadmapStore::setFeedbackExportSha(qint64 reporterId, const QString &sha256,
                                        QString *error) {
    if (!needTx(m_inTransaction, error)) return false;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE feedback_reporter SET export_sha256 = ? WHERE reporter_id = ?"));
    q.addBindValue(nullIfEmpty(sha256));
    q.addBindValue(reporterId);
    return q.exec() || fail(q, error);
}

std::optional<QVector<RoadmapStore::FeedbackFindingRow>>
RoadmapStore::feedbackFindings(qint64 reporterId, QString *error) const {
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT ordinal, heading, heading_line, entry_heading, filed_on, triage, ids"
        " FROM feedback_finding WHERE reporter_id = ? ORDER BY ordinal"));
    q.addBindValue(reporterId);
    if (!q.exec()) {
        fail(q, error);
        return std::nullopt;
    }
    QVector<FeedbackFindingRow> out;
    while (q.next()) {
        FeedbackFindingRow r;
        r.ordinal      = q.value(0).toInt();
        r.heading      = q.value(1).toString();
        r.headingLine  = q.value(2).toInt();
        r.entryHeading = q.value(3).toString();
        r.filedOn      = q.value(4).toString();
        r.triage       = q.value(5).toString();
        const QJsonArray ids =
            QJsonDocument::fromJson(q.value(6).toString().toUtf8()).array();
        for (const auto &v : ids)
            r.ids.append(v.toString());
        out.append(r);
    }
    return out;
}
