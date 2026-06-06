#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// ANTS-2002 — extract the text of a Claude Code message `content` field.
//
// Per the CC JSONL schema a `content` value is EITHER a plain string OR an
// array of typed blocks — e.g. `[{"type":"text","text":"..."}]` (a
// tool_result body can take either form). A bare `QJsonValue::toString()`
// on the array form silently yields "", which previously stranded
// background tasks as "running" forever and made the task list ignore later
// updates. This helper handles both shapes:
//   * string -> returned as-is.
//   * array  -> the `text` of every text-bearing block, newline-joined.
//   * other  -> "".
// Pure (Qt6::Core only) so the two tracker consumers and the tests share one
// implementation.
namespace ClaudeContent {

inline QString toText(const QJsonValue &content) {
    if (content.isString())
        return content.toString();
    if (!content.isArray())
        return QString();
    QString out;
    const QJsonArray blocks = content.toArray();
    for (const QJsonValue &bv : blocks) {
        const QString t =
            bv.toObject().value(QStringLiteral("text")).toString();
        if (t.isEmpty())
            continue;
        if (!out.isEmpty())
            out += QLatin1Char('\n');
        out += t;
    }
    return out;
}

}  // namespace ClaudeContent
