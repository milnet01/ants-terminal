#include "mcpprojection.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace mcp {

bool isFieldProjectionTool(const QString &toolName) {
    return toolName == QStringLiteral("roadmap_query")
        || toolName == QStringLiteral("project_layout")
        || toolName == QStringLiteral("file_outline")
        || toolName == QStringLiteral("get_environment")
        || toolName == QStringLiteral("tab_list")
        || toolName == QStringLiteral("subsystem")
        || toolName == QStringLiteral("git_state")
        || toolName == QStringLiteral("read_log")            // ANTS-1855
        || toolName == QStringLiteral("read_region")         // ANTS-2021
        || toolName == QStringLiteral("codebase_index")      // ANTS-1637
        || toolName == QStringLiteral("model_switch_stats"); // ANTS-1735
}

QString projectFields(const QString &responseText, const QJsonArray &fields) {
    if (fields.isEmpty()) return responseText;

    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(responseText.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return responseText;

    const QJsonObject src = doc.object();
    QJsonObject out;
    for (const QJsonValue &f : fields) {
        if (!f.isString()) continue;
        const QString name = f.toString();
        if (!name.isEmpty() && src.contains(name))
            out.insert(name, src.value(name));
    }
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

namespace {

// ANTS-2086 — name the cheaper mode available on `tool`, given the args
// the caller actually sent (so the nudge isn't shown when they already
// use the lean path). Empty string = no leaner mode to suggest.
QString leanerModeHintFor(const QString &tool, const QJsonObject &args) {
    if (tool == QStringLiteral("roadmap_query")) {
        const QString mode = args.value(QStringLiteral("mode")).toString();
        if (mode.isEmpty() || mode == QStringLiteral("bullets")) {
            if (args.value(QStringLiteral("status")).toString()
                    != QStringLiteral("active"))
                return QStringLiteral(
                    "pass mode=\"headline_only\" (id+status+headline) or "
                    "status=\"active\" / mode=\"section_index\" for a "
                    "much smaller payload");
            return QStringLiteral(
                "pass mode=\"headline_only\" for a ~10x smaller payload");
        }
        return QString();
    }
    if (tool == QStringLiteral("workspace_search"))
        return QStringLiteral(
            "pass max_match_bytes= to clip long match lines, or a "
            "narrower lane=/glob= to cut the result set");
    if (tool == QStringLiteral("file_outline") &&
        !args.contains(QStringLiteral("filter")))
        return QStringLiteral(
            "pass filter=<substr> to scan only matching symbols");
    // Generic fallback: any field-projection tool the caller isn't yet
    // narrowing (ANTS-1720 fields=).
    if (isFieldProjectionTool(tool) &&
        !args.contains(QStringLiteral("fields")))
        return QStringLiteral(
            "pass fields=[…] to return only the fields you need");
    return QString();
}

}  // namespace

QString appendReadHints(const QString &tool, const QJsonObject &args,
                        const QString &responseText, bool etagUnchanged) {
    constexpr int kHintThresholdBytes = 4096;  // only nudge worthwhile bodies
    if (etagUnchanged) return responseText;
    if (args.contains(QStringLiteral("fields"))) return responseText;
    const QByteArray utf8 = responseText.toUtf8();
    if (utf8.size() < kHintThresholdBytes) return responseText;
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(utf8, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject())
        return responseText;
    QJsonObject env = d.object();
    if (!env.value(QStringLiteral("ok")).toBool()) return responseText;
    bool changed = false;
    if (env.contains(QStringLiteral("etag")) &&
        !args.contains(QStringLiteral("etag_match")) &&
        !env.contains(QStringLiteral("next_call_hint"))) {
        env[QStringLiteral("next_call_hint")] = QStringLiteral(
            "pass etag_match=\"%1\" next call to skip an unchanged "
            "re-read (304 Not Modified)")
                .arg(env.value(QStringLiteral("etag")).toString());
        changed = true;
    }
    if (!env.contains(QStringLiteral("leaner_call_hint"))) {
        const QString lean = leanerModeHintFor(tool, args);
        if (!lean.isEmpty()) {
            env[QStringLiteral("leaner_call_hint")] = lean;
            changed = true;
        }
    }
    if (!changed) return responseText;
    return QString::fromUtf8(
        QJsonDocument(env).toJson(QJsonDocument::Compact));
}

}  // namespace mcp
