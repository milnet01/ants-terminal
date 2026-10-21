// ANTS-4659 — see mcpundeclaredargs.h.
#include "mcpundeclaredargs.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "configbackup.h"       // ConfigWriteLock
#include "jsonlfile.h"
#include "modelswitchledger.h"  // nowIso8601

namespace McpUndeclaredArgs {

QString defaultLedgerPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
        + QStringLiteral("/ants-terminal/mcp-undeclared-args.jsonl");
}

QByteArray recordLine(const QString &ts, const QString &tool,
                      const QStringList &keys) {
    QJsonArray kept;
    for (int i = 0; i < keys.size() && i < kMaxKeysPerRecord; ++i)
        kept.append(keys.at(i).left(kMaxKeyChars));
    QJsonObject o;
    o[QStringLiteral("ts")]   = ts;
    o[QStringLiteral("tool")] = tool;
    o[QStringLiteral("keys")] = kept;
    if (keys.size() > kMaxKeysPerRecord)
        o[QStringLiteral("keys_dropped")] = keys.size() - kMaxKeysPerRecord;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool appendRecord(const QString &path, const QString &tool,
                  const QStringList &keys, qint64 capBytes) {
    // Runs on the dispatcher's thread, the GUI thread in the terminal, so the
    // lock waits only kGuiTimeoutMs and then proceeds best-effort, as the
    // model-switch ledgers do (ANTS-1989).
    ConfigWriteLock lock(path, ConfigWriteLock::kGuiTimeoutMs);
    QList<QByteArray> lines = JsonlFile::readLines(path);
    lines.append(recordLine(ModelSwitchLedger::nowIso8601(), tool, keys));
    return JsonlFile::writeLinesAtomic(
        path, JsonlFile::dropOldestToCap(std::move(lines), capBytes));
}

}  // namespace McpUndeclaredArgs
