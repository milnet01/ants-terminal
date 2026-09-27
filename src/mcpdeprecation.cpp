#include "mcpdeprecation.h"

#include "secureio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace mcp {

namespace {

// ANTS-5485 — every verb here is referenced by no current skill, no other
// project's CLAUDE.md and no feedback file (measured 2026-09-27), or has a
// direct replacement. The value is what to use instead.
const QHash<QString, QString> &table() {
    static const QHash<QString, QString> t = {
        {QStringLiteral("get_git_status"),             QStringLiteral("git_state")},
        {QStringLiteral("current_state"),              QStringLiteral("session_orient")},
        {QStringLiteral("session_brief"),              QStringLiteral("session_orient")},
        {QStringLiteral("cross_doc_diff"),             QStringLiteral("the review-contract-set skill")},
        {QStringLiteral("cold_eyes_cross_doc_diff"),   QStringLiteral("the review-contract-set skill")},
        {QStringLiteral("cold_eyes_single_doc"),       QStringLiteral("the review-contract skill")},
        {QStringLiteral("cold_eyes_fold_in"),          QStringLiteral("roadmap_log op:\"append_batch\"")},
        {QStringLiteral("indie_review_orchestrate"),   QStringLiteral("the review-code skill")},
        {QStringLiteral("indie_review_brief"),         QStringLiteral("the review-code skill")},
        {QStringLiteral("indie_review_synthesis_prompt"), QStringLiteral("the review-code skill")},
        {QStringLiteral("indie_review_fold_in"),       QStringLiteral("roadmap_log op:\"append_batch\"")},
        {QStringLiteral("test_audit_fold_in"),         QStringLiteral("roadmap_log op:\"append_batch\"")},
        {QStringLiteral("test_audit_recheck"),         QStringLiteral("the close-findings skill")},
        {QStringLiteral("test_audit_synthesis_prompt"), QStringLiteral("the review-tests skill")},
        {QStringLiteral("debt_sweep_scan"),            QStringLiteral("the debt-sweep skill")},
        {QStringLiteral("debt_sweep_apply_fix"),       QStringLiteral("the debt-sweep skill")},
        {QStringLiteral("debt_sweep_triage_prompt"),   QStringLiteral("the debt-sweep skill")},
        {QStringLiteral("debt_sweep_defer"),           QStringLiteral("roadmap_log op:\"append\"")},
        {QStringLiteral("plan_template"),              QStringLiteral("the write-spec skill")},
        {QStringLiteral("roadmap_branch_drift"),       QStringLiteral("roadmap_query with git_state")},
    };
    return t;
}

}  // namespace

QString deprecatedReplacement(const QString &verb) {
    return table().value(verb);
}

QString deprecationPrefix(const QString &verb) {
    const QString r = deprecatedReplacement(verb);
    if (r.isEmpty()) return QString();
    return QStringLiteral("DEPRECATED (ANTS-5485): use %1 instead; this verb "
                          "is removed after the next release. ").arg(r);
}

QString withDeprecationAdvisory(const QString &responseText, const QString &verb) {
    const QString r = deprecatedReplacement(verb);
    if (r.isEmpty()) return responseText;
    const QJsonDocument doc = QJsonDocument::fromJson(responseText.toUtf8());
    if (!doc.isObject()) return responseText;
    QJsonObject o = doc.object();
    QJsonObject d;
    d[QStringLiteral("replacement")] = r;
    d[QStringLiteral("tracking")]    = QStringLiteral("ANTS-5485");
    o[QStringLiteral("deprecated")]  = d;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString deprecatedCallsPath() {
    // Beside the roadmap store (RoadmapStore::defaultPath's directory).
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/ants-terminal/deprecated-calls.jsonl");
}

void recordDeprecatedCall(const QString &verb, const QString &callerCwd) {
    if (deprecatedReplacement(verb).isEmpty()) return;
    const QString path = deprecatedCallsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool existed = QFileInfo::exists(path);
    QFile f(path);
    // One short line in one write, so appends from several ants-mcpd
    // processes do not interleave (O_APPEND).
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    if (!existed) (void)setOwnerOnlyPerms(f);
    QJsonObject line;
    line[QStringLiteral("at")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    line[QStringLiteral("verb")] = verb;
    if (!callerCwd.isEmpty()) line[QStringLiteral("caller_cwd")] = callerCwd;
    f.write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
}

}  // namespace mcp
