// ANTS-1283 — per-cwd MCP session-memory key-value store. Backed by
// ~/.cache/ants-terminal/mcp-state/<cwd-hash>.json. Lives in
// ants_core_lib (Qt6::Core only). See docs/specs/ANTS-1283.md.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace SessionMemoryEngine {

constexpr qint64 kMaxStoreBytes = 100 * 1024;   // INV-2 — 100 KiB total cap
constexpr qint64 kMaxValueBytes =  16 * 1024;   // INV-8 — 16 KiB per value
constexpr int    kMaxKeyLength  = 64;           // INV-7 — key length
// Indie-review-2026-05-14 lane-5 HI-4: cap distinct-key count. Byte
// caps don't bound the QJsonObject member count, which dominates the
// List-op O(n log n) sort cost. 1024 is comfortably above realistic
// use yet well below worst-case pressure.
constexpr int    kMaxStoreKeys  = 1024;

enum class Op { Get, Set, Delete, List };

struct OpResult {
    bool        ok            = false;
    QString     code;          // empty on success; see spec § 2.3 otherwise
    QString     error;
    QString     op;            // echo of the op for the caller
    QString     key;
    bool        found         = false;
    QJsonValue  value;
    qint64      bytesWritten  = 0;
    qint64      totalBytes    = 0;
    QJsonArray  keys;          // [{key, bytes}, ...] for List
    QString     path;          // absolute path to the store file
};

QString cwdHash(const QString &absoluteCwd);
QString defaultBaseDir();
QString storePathFor(const QString &cwd, const QString &baseDirOverride = {});

bool        parseOp(const QString &raw, Op *out);
bool        isValidKey(const QString &key);
QJsonObject loadStore(const QString &path, bool *corruptOut = nullptr);
// ANTS-1364 — accepts pre-serialized bytes so callers that already
// hold them (cap-check call sites) avoid a redundant
// QJsonDocument(store).toJson(Compact). Pre-fix signature took a
// QJsonObject and serialized internally, doubling the work.
QString     saveStore(const QString &path, const QByteArray &body);
qint64      serializedSize(const QJsonObject &store);
qint64      serializedSize(const QJsonValue &value);

OpResult    execute(const QString &cwd, Op op,
                    const QString &key, const QJsonValue &value,
                    const QString &baseDirOverride = {});

}  // namespace SessionMemoryEngine
