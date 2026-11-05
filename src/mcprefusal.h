#pragma once

// ANTS-2216 — the MCP refusal envelope, {ok:false, code, error}, in one
// place. The error codes themselves: docs/standards/mcp-error-codes.md.

#include <QJsonObject>
#include <QString>

namespace mcp {

inline QJsonObject refusal(const QString &code, const QString &message) {
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), code},
                       {QStringLiteral("error"), message}};
}

}  // namespace mcp
