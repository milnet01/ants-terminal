#pragma once

// ANTS-6829 — tool_info {name, op}: the part of one verb's descriptor that
// `op` uses, for a verb whose whole descriptor is too large to read. The
// verb's `op` argument must list `op` in its enum; otherwise the reply refuses
// `bad_args` (the verb has no `op` argument) or `unknown_op` with `ops[]`.
//
// The slice keeps `description` and `selection_hint` whole, and adds:
//  - `op_detail`: the sentences of `detail` and of the `op` argument that
//    belong to `op`. A sentence naming a quoted op ("append") starts a run;
//    the sentences after it that name none belong to the same run.
//  - `inputSchema.properties`: each argument whose description names `op`
//    quoted, or names no op at all. The `op` argument is left out: its text
//    is in `op_detail`.
//  - `other_arguments`: the names left out, which the whole descriptor
//    (tool_info without `op`) still holds.
//
// Reload story: compiled in, so a change reaches sessions with an ants-mcpd
// rebuild and an MCP reconnect; the terminal is not relaunched.

#include <QJsonObject>
#include <QString>

namespace mcp {

QJsonObject toolInfoForOp(const QJsonObject &descriptor, const QString &op);

}  // namespace mcp
