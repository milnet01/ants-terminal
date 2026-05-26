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

}  // namespace mcp
