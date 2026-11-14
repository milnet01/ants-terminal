// ANTS-4659 — ledger of undeclared MCP arguments.
//
// The dispatcher refuses a call naming an argument the verb's schema does not
// declare. This ledger keeps one line per such call. It began as the survey
// that decided the refusal, and now shows what callers still get refused for.
//
// A line holds the time, the verb and the argument NAMES. Values are never
// stored. Only a process that sets a path writes (ClaudeIntegration::
// setUndeclaredArgsLedgerPath), so tests that dispatch write nothing.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace McpUndeclaredArgs {

constexpr qint64 kMaxLedgerBytes   = qint64{256} * 1024;  // oldest lines drop past this
constexpr int    kMaxKeysPerRecord = 16;          // a caller cannot grow one line
constexpr int    kMaxKeyChars      = 64;          // without bound

// ~/.cache/ants-terminal/mcp-undeclared-args.jsonl
QString defaultLedgerPath();

// One compact JSON line: {"ts","tool","keys"}, plus "keys_dropped" when more
// than kMaxKeysPerRecord keys were sent. A key over kMaxKeyChars is cut. Pure.
QByteArray recordLine(const QString &ts, const QString &tool,
                      const QStringList &keys);

// Append one record for `tool` and drop the oldest lines past capBytes.
// The read-modify-write holds a ConfigWriteLock, because the terminal and
// ants-mcpd both append to the same file.
bool appendRecord(const QString &path, const QString &tool,
                  const QStringList &keys, qint64 capBytes = kMaxLedgerBytes);

}  // namespace McpUndeclaredArgs
