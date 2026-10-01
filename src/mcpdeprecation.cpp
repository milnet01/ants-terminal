#include "mcpdeprecation.h"

#include <QHash>

namespace mcp {

namespace {

// ANTS-5485 — the twenty verbs removed after a release in which none was
// called. The value is what to use instead.
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

QString removedReplacement(const QString &verb) {
    return table().value(verb);
}

QJsonObject removedVerbError(const QString &verb) {
    const QString r = removedReplacement(verb);
    if (r.isEmpty()) return QJsonObject();
    QJsonObject data;
    data[QStringLiteral("code")]        = QStringLiteral("verb_removed");
    data[QStringLiteral("replacement")] = r;
    QJsonObject e;
    e[QStringLiteral("code")]    = -32602;
    e[QStringLiteral("message")] =
        QStringLiteral("Tool %1 was removed (ANTS-5485); use %2 instead.")
            .arg(verb, r);
    e[QStringLiteral("data")]    = data;
    return e;
}

}  // namespace mcp
