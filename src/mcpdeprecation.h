#pragma once

// ANTS-5485 — the record of the MCP verbs that were removed, and what
// replaced each. The table is never listed in tools/list, so it costs a
// session nothing; it is read only when a call names a verb no server
// registers, so that call is told what to use instead.
//
// Reload story: the table is compiled in, so a change reaches sessions with an
// ants-mcpd rebuild and an MCP reconnect; the terminal is not relaunched.

#include <QJsonObject>
#include <QString>

namespace mcp {

// What replaced a removed verb; empty for any other name. Every entry names
// a verb or a skill.
QString removedReplacement(const QString &verb);

// The JSON-RPC error a call to a removed verb gets:
// {code:-32602, message, data:{code:"verb_removed", replacement}}.
// An empty object for any other name.
QJsonObject removedVerbError(const QString &verb);

}  // namespace mcp
